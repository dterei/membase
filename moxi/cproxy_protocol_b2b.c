/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <assert.h>
#include <math.h>
#include "memcached.h"
#include "cproxy.h"
#include "work.h"
#include "log.h"

// Internal declarations.
//
static protocol_binary_request_noop req_noop = {
    .bytes = {0}
};

void cproxy_init_b2b() {
    memset(&req_noop, 0, sizeof(req_noop));

    req_noop.message.header.request.magic    = PROTOCOL_BINARY_REQ;
    req_noop.message.header.request.opcode   = PROTOCOL_BINARY_CMD_NOOP;
    req_noop.message.header.request.datatype = PROTOCOL_BINARY_RAW_BYTES;
}

/* Do the actual work of forwarding the command from an
 * upstream binary conn to its assigned binary downstream.
 */
bool cproxy_forward_b2b_downstream(downstream *d) {
    assert(d != NULL);
    assert(d->ptd != NULL);
    assert(d->ptd->proxy != NULL);
    assert(d->downstream_conns != NULL);
    assert(d->downstream_used == 0);
    assert(d->multiget == NULL);
    assert(d->merger == NULL);

    d->downstream_used_start = 0;

    conn *uc = d->upstream_conn;

    if (settings.verbose > 2) {
        moxi_log_write("%d: cproxy_forward_b2b_downstream %x\n",
                uc->sfd, uc->cmd);
    }

    assert(uc != NULL);
    assert(uc->state == conn_pause);
    assert(uc->cmd >= 0);
    assert(uc->cmd_start == NULL);
    assert(uc->thread != NULL);
    assert(uc->thread->base != NULL);
    assert(uc->noreply == false);
    assert(IS_BINARY(uc->protocol));
    assert(IS_PROXY(uc->protocol));

    int server_index = -1;

    if (cproxy_is_broadcast_cmd(uc->cmd) == false &&
        uc->corked == NULL) {
        item *it = uc->item;
        assert(it != NULL);

        protocol_binary_request_header *req =
            (protocol_binary_request_header *) ITEM_data(it);

        char *key     = ((char *) req) + sizeof(*req) + req->request.extlen;
        int   key_len = ntohs(req->request.keylen);

        if (key_len > 0) {
            server_index = cproxy_server_index(d, key, key_len, NULL);
            if (server_index < 0) {
                return false;
            }
        }
    }

    int nc = cproxy_connect_downstream(d, uc->thread, server_index);
    if (nc == -1) {
        return true;
    }

    if (nc > 0) {
        assert(d->downstream_conns != NULL);

        if (d->usec_start == 0 &&
            d->ptd->behavior_pool.base.time_stats) {
            d->usec_start = usec_now();
        }

        int nconns = mcs_server_count(&d->mst);

        for (int i = 0; i < nconns; i++) {
            conn *c = d->downstream_conns[i];
            if (c != NULL &&
                c != NULL_CONN) {
                assert(c->state == conn_pause);
                assert(c->item == NULL);

                if (cproxy_prep_conn_for_write(c) == false) {
                    d->ptd->stats.stats.err_downstream_write_prep++;
                    cproxy_close_conn(c);

                    return false;
                }
            }
        }

        // Uncork the saved-up quiet binary commands.
        //
        cproxy_binary_uncork_cmds(d, uc);

        if (uc->cmd == PROTOCOL_BINARY_CMD_FLUSH ||
            uc->cmd == PROTOCOL_BINARY_CMD_NOOP ||
            uc->cmd == PROTOCOL_BINARY_CMD_STAT) {
            return cproxy_broadcast_b2b_downstream(d, uc);
        }

        return cproxy_forward_b2b_simple_downstream(d, uc);
    }

    if (settings.verbose > 2) {
        moxi_log_write("%d: cproxy_forward_b2b_downstream connect failed\n",
                uc->sfd);
    }

    return false;
}

/* A simple command includes a key, for hashing.
 */
bool cproxy_forward_b2b_simple_downstream(downstream *d, conn *uc) {
    return b2b_forward_item(uc, d, uc->item);
}

bool b2b_forward_item(conn *uc, downstream *d, item *it) {
    assert(uc != NULL);
    assert(uc->next == NULL);
    assert(uc->noreply == false);
    assert(it != NULL);

    protocol_binary_request_header *req =
        (protocol_binary_request_header *) ITEM_data(it);

    char *key    = ((char *) req) + sizeof(*req) + req->request.extlen;
    int   keylen = ntohs(req->request.keylen);

    if (settings.verbose > 2) {
        char buf[300];
        memcpy(buf, key, keylen);
        buf[keylen] = '\0';

        moxi_log_write("%d: b2b_forward_item nbytes %u, extlen %d, keylen %d opcode %x key (%s)\n",
                       uc->sfd, it->nbytes, req->request.extlen, keylen, req->request.opcode, buf);

        cproxy_dump_header(uc->sfd, (char *) req);
    }

    if (key == NULL ||
        keylen <= 0) {
        return false; // We don't know how to hash an empty key.
    }

    int  vbucket = -1;
    bool local;

    conn *c = cproxy_find_downstream_conn_ex(d, key, keylen,
                                             &local, &vbucket);
    if (c != NULL) {
        if (local) {
            uc->hit_local = true;
        }
        if (b2b_forward_item_vbucket(uc, d, it, c, vbucket) == true) {
            d->downstream_used_start = 1;
            d->downstream_used = 1;

            cproxy_start_downstream_timeout(d, c);

            return true;
        }
    }

    if (settings.verbose > 2) {
        moxi_log_write("%d: b2b_forward_item failed (%d)\n",
                uc->sfd, (c != NULL));
    }

    return false;
}

bool b2b_forward_item_vbucket(conn *uc, downstream *d, item *it,
                              conn *c, int vbucket) {
    assert(d != NULL);
    assert(d->ptd != NULL);
    assert(uc != NULL);
    assert(uc->next == NULL);
    assert(uc->noreply == false);
    assert(c != NULL);

    // Assuming we're already connected to downstream.
    //
    if (settings.verbose > 2) {
        moxi_log_write("%d: b2b_forward_item_vbucket %x to %d, vbucket %d\n",
                uc->sfd, uc->cmd, c->sfd, vbucket);
    }

    protocol_binary_request_header *req =
        (protocol_binary_request_header *) ITEM_data(it);

    if (vbucket >= 0) {
        req->request.reserved = htons(vbucket);
    }

    if (add_conn_item(c, it) == true) {
        // The caller keeps its refcount, and we need our own.
        //
        it->refcount++;

        if (add_iov(c, ITEM_data(it), it->nbytes) == 0) {
            conn_set_state(c, conn_mwrite);
            c->write_and_go = conn_new_cmd;

            if (update_event(c, EV_WRITE | EV_PERSIST)) {
                if (settings.verbose > 2) {
                    moxi_log_write("%d: b2b_forward %x to %d success\n",
                            uc->sfd, uc->cmd, c->sfd);
                }

                return true;
            }
        }
    }

    d->ptd->stats.stats.err_oom++;
    cproxy_close_conn(c);

    return false;
}

/* Used for broadcast commands, like no-op, flush_all or stats.
 */
bool cproxy_broadcast_b2b_downstream(downstream *d, conn *uc) {
    assert(d != NULL);
    assert(d->ptd != NULL);
    assert(d->ptd->proxy != NULL);
    assert(d->downstream_conns != NULL);
    assert(uc != NULL);
    assert(uc->next == NULL);
    assert(uc->noreply == false);

    int nwrite = 0;
    int nconns = mcs_server_count(&d->mst);

    for (int i = 0; i < nconns; i++) {
        conn *c = d->downstream_conns[i];
        if (c != NULL &&
            c != NULL_CONN &&
            b2b_forward_item_vbucket(uc, d, uc->item, c, -1) == true) {
            nwrite++;
        }
    }

    if (settings.verbose > 2) {
        moxi_log_write("%d: b2b broadcast nwrite %d out of %d\n",
                uc->sfd, nwrite, nconns);
    }

    if (nwrite > 0) {
        // TODO: Handle binary 'stats reset' sub-command.
        //
        if (uc->cmd == PROTOCOL_BINARY_CMD_STAT &&
            d->merger == NULL) {
            d->merger = genhash_init(128, skeyhash_ops);
        }

        item *it = item_alloc("h", 1, 0, 0,
                              sizeof(protocol_binary_response_header));
        if (it != NULL) {
            protocol_binary_response_header *header =
                (protocol_binary_response_header *) ITEM_data(it);

            memset(ITEM_data(it), 0, it->nbytes);

            header->response.magic  = (uint8_t) PROTOCOL_BINARY_RES;
            header->response.opcode = uc->binary_header.request.opcode;
            header->response.opaque = uc->opaque;

            if (add_conn_item(uc, it)) {
                d->upstream_suffix     = ITEM_data(it);
                d->upstream_suffix_len = it->nbytes;
                d->upstream_status = PROTOCOL_BINARY_RESPONSE_SUCCESS;
                d->target_host_ident = NULL;

                if (settings.verbose > 2) {
                    moxi_log_write("%d: b2b broadcast upstream_suffix", uc->sfd);
                    cproxy_dump_header(uc->sfd, ITEM_data(it));
                }

                // TODO: Handle FLUSHQ (quiet binary flush_all).
                //
                d->downstream_used_start = nwrite;
                d->downstream_used       = nwrite;

                cproxy_start_downstream_timeout(d, NULL);

                return true;
            }

            item_remove(it);
        }
    }

    return false;
}

/* Called when we receive a binary response header from
 * a downstream server, via try_read_command()/drive_machine().
 */
void cproxy_process_b2b_downstream(conn *c) {
    assert(c != NULL);
    assert(c->cmd >= 0);
    assert(c->next == NULL);
    assert(c->item == NULL);
    assert(IS_BINARY(c->protocol));
    assert(IS_PROXY(c->protocol));
    assert(c->substate == bin_no_state);

    downstream *d = c->extra;
    assert(d);

    c->cmd_curr       = -1;
    c->cmd_start      = NULL;
    c->cmd_start_time = msec_current_time;
    c->cmd_retries    = 0;

    int      extlen  = c->binary_header.request.extlen;
    int      keylen  = c->binary_header.request.keylen;
    uint32_t bodylen = c->binary_header.request.bodylen;

    if (settings.verbose > 2) {
        moxi_log_write("<%d cproxy_process_b2b_downstream %x %d %d %u\n",
                c->sfd, c->cmd, extlen, keylen, bodylen);
    }

    assert(bodylen >= (uint32_t) keylen + extlen);

    process_bin_noreply(c); // Map quiet c->cmd values into non-quiet.

    // Our approach is to read everything we can before
    // getting into big switch/case statements for the
    // actual processing.
    //
    // Alloc an item and continue with an rest-of-body nread if
    // necessary.  The item will hold the entire response message
    // (the header + body).
    //
    char *ikey    = "q";
    int   ikeylen = 1;

    c->item = item_alloc(ikey, ikeylen, 0, 0,
                         sizeof(c->binary_header) + bodylen);
    if (c->item != NULL) {
        item *it = c->item;
        void *rb = c->rcurr;

        assert(it->refcount == 1);

        memcpy(ITEM_data(it), rb, sizeof(c->binary_header));

        if (bodylen > 0) {
            c->ritem = ITEM_data(it) + sizeof(c->binary_header);
            c->rlbytes = bodylen;
            c->substate = bin_read_set_value;

            conn_set_state(c, conn_nread);
        } else {
            // Since we have no body bytes, we can go immediately to
            // the nread completed processing step.
            //
            cproxy_process_b2b_downstream_nread(c);
        }
    } else {
        d->ptd->stats.stats.err_oom++;
        cproxy_close_conn(c);
    }
}

/* We reach here after nread'ing a header+body into an item.
 */
void cproxy_process_b2b_downstream_nread(conn *c) {
    assert(c != NULL);
    assert(c->cmd >= 0);
    assert(c->next == NULL);
    assert(c->cmd_start == NULL);
    assert(IS_BINARY(c->protocol));
    assert(IS_PROXY(c->protocol));

    protocol_binary_response_header *header =
        (protocol_binary_response_header *) &c->binary_header;

    int      extlen  = header->response.extlen;
    int      keylen  = header->response.keylen;
    uint32_t bodylen = header->response.bodylen;
    int      status  = ntohs(header->response.status);
    int      opcode  = header->response.opcode;

    if (settings.verbose > 2) {
        moxi_log_write("<%d cproxy_process_b2b_downstream_nread %x %x %d %d %u %d %x\n",
                c->sfd, c->cmd, opcode, extlen, keylen, bodylen, c->noreply, status);
    }

    downstream *d = c->extra;
    assert(d != NULL);
    assert(d->ptd != NULL);
    assert(d->ptd->proxy != NULL);

    // TODO: Need to handle quiet binary command error response,
    //       in the right order.
    // TODO: Need to handle not-my-vbucket error during a quiet cmd.
    //
    conn *uc = d->upstream_conn;
    item *it = c->item;

    // Clear c->item because we either move it to the upstream or
    // item_remove() it on error.
    //
    c->item = NULL;

    assert(it != NULL);
    assert(it->refcount == 1);

    if (cproxy_binary_ignore_reply(c, header, it)) {
        return;
    }

    if (c->noreply) {
        conn_set_state(c, conn_new_cmd);
    } else {
        conn_set_state(c, conn_pause);

        if (opcode == PROTOCOL_BINARY_CMD_NOOP ||
            opcode == PROTOCOL_BINARY_CMD_FLUSH) {
            goto done;
        }

        if (opcode == PROTOCOL_BINARY_CMD_STAT) {
            if (status == PROTOCOL_BINARY_RESPONSE_SUCCESS) {
                if (keylen > 0) {
                    if (d->merger != NULL) {
                        char *key = (ITEM_data(it)) + sizeof(*header) + extlen;
                        char *val = key + keylen;

                        protocol_stats_merge_name_val(d->merger, "STAT", 4,
                                                      key, keylen,
                                                      val, bodylen - keylen - extlen);
                    }

                    conn_set_state(c, conn_new_cmd); // Get next STATS response.
                }
            }

            goto done;
        }

        // If the client is still there, we should handle
        // a not-my-vbucket error with a possible retry.
        //
        if (uc != NULL &&
            status == PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET) {
            if (settings.verbose > 2) {
                moxi_log_write("<%d cproxy_process_b2b_downstream_nread not-my-vbucket, "
                        "cmd: %x %d\n",
                        c->sfd, header->response.opcode, uc->item != NULL);
            }

            assert(uc->item != NULL);

            protocol_binary_request_header *req =
                (protocol_binary_request_header *) ITEM_data((item *) uc->item);

            int vbucket = ntohs(req->request.reserved);
            int sindex = downstream_conn_index(d, c);

            if (settings.verbose > 2) {
                moxi_log_write("<%d cproxy_process_b2b_downstream_nread not-my-vbucket, "
                        "cmd: %x not multi-key get, sindex %d, vbucket %d, retries %d\n",
                        c->sfd, header->response.opcode,
                        sindex, vbucket, uc->cmd_retries);
            }

            mcs_server_invalid_vbucket(&d->mst, sindex, vbucket);

            // As long as the upstream is still open and we haven't
            // retried too many times already.
            //
            int max_retries = cproxy_max_retries(d);

            if (uc->cmd_retries < max_retries) {
                uc->cmd_retries++;

                d->upstream_retry++;
                d->ptd->stats.stats.tot_retry_vbucket++;

                goto done;
            }

            if (settings.verbose > 2) {
                moxi_log_write("%d: cproxy_process_b2b_downstream_nread not-my-vbucket, "
                        "cmd: %x skipping retry %d >= %d\n",
                        c->sfd, header->response.opcode, uc->cmd_retries,
                        max_retries);
            }
        }
    }

    // Write the response to the upstream connection.
    //
    if (uc != NULL) {
        if (settings.verbose > 2) {
            moxi_log_write("<%d cproxy_process_b2b_downstream_nread got %u\n",
                           c->sfd, it->nbytes);

            cproxy_dump_header(c->sfd, ITEM_data(it));
        }

        if (add_conn_item(uc, it) == true) {
            it->refcount++;

            if (add_iov(uc, ITEM_data(it), it->nbytes) == 0) {
                // If we got a quiet response, however, don't change the
                // upstream connection's state (should be in paused state),
                // as we expect the downstream server to provide a
                // verbal/non-quiet response that moves the downstream
                // conn through the conn_pause countdown codepath.
                //
                if (c->noreply == false) {
                    cproxy_update_event_write(d, uc);

                    conn_set_state(uc, conn_mwrite);
                }

                goto done;
            }
        }

        d->ptd->stats.stats.err_oom++;
        cproxy_close_conn(uc);
    }

 done:
    if (it != NULL) {
        item_remove(it);
    }
}

