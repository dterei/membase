/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <assert.h>
#include "memcached.h"
#include "cproxy.h"
#include "work.h"
#include "log.h"

// Internal declarations.
//
#define COMMAND_TOKEN 0
#define MAX_TOKENS    8

#define MAX_HOSTNAME_LEN 200
#define MAX_PORT_LEN     8

void cproxy_process_upstream_ascii(conn *c, char *line) {
    assert(c != NULL);
    assert(c->next == NULL);
    assert(c->extra != NULL);
    assert(c->cmd == -1);
    assert(c->item == NULL);
    assert(line != NULL);
    assert(line == c->rcurr);
    assert(IS_ASCII(c->protocol));
    assert(IS_PROXY(c->protocol));

    if (settings.verbose > 2) {
        moxi_log_write("<%d cproxy_process_upstream_ascii %s\n",
                       c->sfd, line);
    }

    // Snapshot rcurr, because the caller, try_read_command(), changes it.
    //
    c->cmd_curr       = -1;
    c->cmd_start      = c->rcurr;
    c->cmd_start_time = msec_current_time;
    c->cmd_retries    = 0;

    proxy_td *ptd = c->extra;
    assert(ptd != NULL);

    /* For commands set/add/replace, we build an item and read the data
     * directly into it, then continue in nread_complete().
     */
    if (!cproxy_prep_conn_for_write(c)) {
        ptd->stats.stats.err_upstream_write_prep++;
        conn_set_state(c, conn_closing);
        return;
    }

    bool mcmux_command = false;
    bool self_command = false;

    /* Check for proxy pattern - A:host:port or B:host:port */
    if (true == settings.enable_mcmux_mode &&
        ((*line == 'A' || *line == 'B') && *(line + 1) == ':')) {
        mcmux_command = true;
    } else if (true == settings.enable_mcmux_mode) {
        self_command = true;
    }

    c->peer_protocol = 0;
    c->peer_host = NULL;
    c->peer_port = 0;

    if (mcmux_command) {
        char *peer_port = NULL;
        int i = 0;

        c->peer_protocol = (*line == 'A') ?
            proxy_downstream_ascii_prot :
            proxy_downstream_binary_prot;
        line += 2;
        c->peer_host = line;

        while (*line != ' ' && *line != '\0' &&
               *line != ':' && ++i < MAX_HOSTNAME_LEN) {
            line++;
        }

        if (*line == '\0' || line - c->peer_host <= 0) {
            out_string(c, "ERROR");
            moxi_log_write("Malformed request line");
            return;
        }
        *line = '\0';
        line++;
        peer_port = line;
        i = 0;

        while (*line != ' ' && *line != '\0' && ++i <= MAX_PORT_LEN) {
            line++;
        }

        if (*line == '\0' || line - peer_port <= 0) {
            out_string(c, "ERROR");
            moxi_log_write("Malformed request line");
            return;
        }

        c->peer_port = atoi(peer_port);

        *line++ = '\0';
        c->cmd_start = line;
    }

    int     cmd_len = 0;
    token_t tokens[MAX_TOKENS];
    size_t  ntokens = scan_tokens(line, tokens, MAX_TOKENS, &cmd_len);
    char   *cmd     = tokens[COMMAND_TOKEN].value;
    int     cmdx    = -1;
    int     cmd_st  = STATS_CMD_TYPE_REGULAR;
    int     comm;

#define SEEN(cmd_id, is_cas, cmd_len)                           \
    cmd_st = c->noreply ?                                       \
        STATS_CMD_TYPE_QUIET : STATS_CMD_TYPE_REGULAR;          \
    ptd->stats.stats_cmd[cmd_st][cmd_id].seen++;                \
    ptd->stats.stats_cmd[cmd_st][cmd_id].read_bytes += cmd_len; \
    if (is_cas) {                                               \
        ptd->stats.stats_cmd[cmd_st][cmd_id].cas++;             \
    }

    if (ntokens >= 3 &&
        (false == self_command) &&
        (strncmp(cmd, "get", 3) == 0)) {
        if (cmd[3] == 'l') {
            c->cmd_curr = PROTOCOL_BINARY_CMD_GETL;
        } else if (ntokens == 3) {
            // Single-key get/gets optimization.
            //
            c->cmd_curr = PROTOCOL_BINARY_CMD_GETK;
        } else {
            c->cmd_curr = PROTOCOL_BINARY_CMD_GETKQ;
        }

        // Handles get and gets.
        //
        cproxy_pause_upstream_for_downstream(ptd, c);

        // The cmd_len from scan_tokens might not include
        // all the keys, so cmd_len might not == strlen(command).
        // Handle read_bytes during multiget broadcast.
        //
        if (cmd[3] == 'l') {
            SEEN(STATS_CMD_GETL, true, 0);
        } else {
            SEEN(STATS_CMD_GET, cmd[3] == 's', 0);
        }

    } else if ((ntokens == 6 || ntokens == 7) &&
                (false == self_command) &&
               ((strncmp(cmd, "add", 3) == 0 &&
                 (comm = NREAD_ADD) &&
                 (cmdx = STATS_CMD_ADD) &&
                 (c->cmd_curr = PROTOCOL_BINARY_CMD_ADD)) ||
                (strncmp(cmd, "set", 3) == 0 &&
                 (comm = NREAD_SET) &&
                 (cmdx = STATS_CMD_SET) &&
                 (c->cmd_curr = PROTOCOL_BINARY_CMD_SET)) ||
                (strncmp(cmd, "replace", 7) == 0 &&
                 (comm = NREAD_REPLACE) &&
                 (cmdx = STATS_CMD_REPLACE) &&
                 (c->cmd_curr = PROTOCOL_BINARY_CMD_REPLACE)) ||
                (strncmp(cmd, "prepend", 7) == 0 &&
                 (comm = NREAD_PREPEND) &&
                 (cmdx = STATS_CMD_PREPEND) &&
                 (c->cmd_curr = PROTOCOL_BINARY_CMD_PREPEND)) ||
                (strncmp(cmd, "append", 6) == 0 &&
                 (comm = NREAD_APPEND) &&
                 (cmdx = STATS_CMD_APPEND) &&
                 (c->cmd_curr = PROTOCOL_BINARY_CMD_APPEND)))) {
        assert(c->item == NULL);
        c->item = NULL;

        process_update_command(c, tokens, ntokens, comm, false);

        if (cmdx >= 0) {
            item *it = c->item;
            if (it != NULL) {
                SEEN(cmdx, false, cmd_len + it->nbytes);
            } else {
                SEEN(cmdx, false, cmd_len);
                ptd->stats.stats_cmd[cmd_st][cmdx].misses++;
            }
        }

    } else if ((ntokens == 7 || ntokens == 8) &&
               (false == self_command) &&
               (strncmp(cmd, "cas", 3) == 0 &&
                (comm = NREAD_CAS) &&
                (c->cmd_curr = PROTOCOL_BINARY_CMD_SET))) {
        assert(c->item == NULL);
        c->item = NULL;

        process_update_command(c, tokens, ntokens, comm, true);

        item *it = c->item;
        if (it != NULL) {
            SEEN(STATS_CMD_CAS, true, cmd_len + it->nbytes);
        } else {
            SEEN(STATS_CMD_CAS, true, cmd_len);
            ptd->stats.stats_cmd[cmd_st][STATS_CMD_CAS].misses++;
        }

    } else if ((ntokens == 4 || ntokens == 5) &&
               (false == self_command) &&
               (strncmp(cmd, "incr", 4) == 0) &&
               (c->cmd_curr = PROTOCOL_BINARY_CMD_INCREMENT)) {
        set_noreply_maybe(c, tokens, ntokens);
        cproxy_pause_upstream_for_downstream(ptd, c);

        SEEN(STATS_CMD_INCR, false, cmd_len);

    } else if ((ntokens == 4 || ntokens == 5) &&
               (false == self_command) &&
               (strncmp(cmd, "decr", 4) == 0) &&
               (c->cmd_curr = PROTOCOL_BINARY_CMD_DECREMENT)) {
        set_noreply_maybe(c, tokens, ntokens);
        cproxy_pause_upstream_for_downstream(ptd, c);

        SEEN(STATS_CMD_DECR, false, cmd_len);

    } else if (ntokens >= 3 && ntokens <= 4 &&
               (false == self_command) &&
               (strncmp(cmd, "delete", 6) == 0) &&
               (c->cmd_curr = PROTOCOL_BINARY_CMD_DELETE)) {
        set_noreply_maybe(c, tokens, ntokens);
        cproxy_pause_upstream_for_downstream(ptd, c);

        SEEN(STATS_CMD_DELETE, false, cmd_len);

    } else if (ntokens >= 2 && ntokens <= 4 &&
               (false == self_command) &&
               (strncmp(cmd, "flush_all", 9) == 0) &&
               (c->cmd_curr = PROTOCOL_BINARY_CMD_FLUSH)) {
        set_noreply_maybe(c, tokens, ntokens);
        cproxy_pause_upstream_for_downstream(ptd, c);

        SEEN(STATS_CMD_FLUSH_ALL, false, cmd_len);

    } else if (ntokens >= 3 && ntokens <= 4 &&
               (strncmp(cmd, "stats proxy", 10) == 0)) {

        process_stats_proxy_command(c, tokens, ntokens);

        SEEN(STATS_CMD_STATS, false, cmd_len);

    } else if (ntokens == 3 &&
               (false == self_command) &&
               (strcmp(cmd, "stats reset") == 0) &&
               (c->cmd_curr = PROTOCOL_BINARY_CMD_STAT)) {
        cproxy_pause_upstream_for_downstream(ptd, c);

        SEEN(STATS_CMD_STATS_RESET, false, cmd_len);

    } else if (ntokens == 2 &&
               (false == self_command) &&
               (strcmp(cmd, "stats") == 0) &&
               (c->cmd_curr = PROTOCOL_BINARY_CMD_STAT)) {
        // Even though we've coded to handle advanced stats
        // like stats cachedump, prevent those here to avoid
        // locking downstream servers.
        //
        cproxy_pause_upstream_for_downstream(ptd, c);

        SEEN(STATS_CMD_STATS, false, cmd_len);

    } else if (ntokens == 2 &&
               (true == mcmux_command) &&
               (strncmp(cmd, "version", 7) == 0) &&
               (c->cmd_curr = PROTOCOL_BINARY_CMD_VERSION)) {
        /* downstream version command */
        cproxy_pause_upstream_for_downstream(ptd, c);

        SEEN(STATS_CMD_VERSION, false, cmd_len);

    } else if (ntokens == 2 &&
               (strncmp(cmd, "version", 7) == 0)) {
        out_string(c, "VERSION " VERSION);

        SEEN(STATS_CMD_VERSION, false, cmd_len);

    } else if ((ntokens == 3 || ntokens == 4) &&
               (strncmp(cmd, "verbosity", 9) == 0)) {
        process_verbosity_command(c, tokens, ntokens);

        SEEN(STATS_CMD_VERBOSITY, false, cmd_len);

    } else if (ntokens == 2 &&
               (strncmp(cmd, "quit", 4) == 0)) {
        conn_set_state(c, conn_closing);

        SEEN(STATS_CMD_QUIT, false, cmd_len);

    } else if (ntokens == 4 &&
               (strncmp(cmd, "unl", 3) == 0) &&
               (false == self_command) &&
               (c->cmd_curr = PROTOCOL_BINARY_CMD_UNL)) {
        cproxy_pause_upstream_for_downstream(ptd, c);

        SEEN(STATS_CMD_UNL, false, cmd_len);

    } else if (ntokens == 4 && // Ex: "touch <key> <expiration>"
               (false == self_command) &&
               (strncmp(cmd, "touch", 5) == 0) &&
               (c->cmd_curr = PROTOCOL_BINARY_CMD_TOUCH)) {
        cproxy_pause_upstream_for_downstream(ptd, c);

        // TODO: SEEN(STATS_CMD_TOUCH, false, cmd_len);

    } else {
        out_string(c, "ERROR");

        SEEN(STATS_CMD_ERROR, false, cmd_len);
    }
}

/* We get here after reading the value in set/add/replace
 * commands. The command has been stored in c->cmd, and
 * the item is ready in c->item.
 */
void cproxy_process_upstream_ascii_nread(conn *c) {
    assert(c != NULL);
    assert(c->next == NULL);

    item *it = c->item;

    assert(it != NULL);

    // pthread_mutex_lock(&c->thread->stats.mutex);
    // c->thread->stats.slab_stats[it->slabs_clsid].set_cmds++;
    // pthread_mutex_unlock(&c->thread->stats.mutex);

    if (strncmp(ITEM_data(it) + it->nbytes - 2, "\r\n", 2) == 0) {
        proxy_td *ptd = c->extra;

        assert(ptd != NULL);

        cproxy_pause_upstream_for_downstream(ptd, c);
    } else {
        out_string(c, "CLIENT_ERROR bad data chunk");
    }
}

/**
 * @param cas_emit  1: emit CAS.
 *                  0: do not emit CAS.
 *                 -1: data driven.
 */
void cproxy_upstream_ascii_item_response(item *it, conn *uc,
                                         int cas_emit) {
    assert(it != NULL);
    assert(uc != NULL);
    assert(uc->state == conn_pause);
    assert(uc->funcs != NULL);
    assert(IS_ASCII(uc->protocol));
    assert(IS_PROXY(uc->protocol));

    if (settings.verbose > 2) {
        char key[KEY_MAX_LENGTH + 10];
        assert(it->nkey <= KEY_MAX_LENGTH);
        memcpy(key, ITEM_key(it), it->nkey);
        key[it->nkey] = '\0';

        moxi_log_write("<%d cproxy ascii item response, key %s\n",
                       uc->sfd, key);
    }

    if (strncmp(ITEM_data(it) + it->nbytes - 2, "\r\n", 2) == 0) {
        // TODO: Need to clean up half-written add_iov()'s.
        //       Consider closing the upstream_conns?
        //
        uint64_t cas = ITEM_get_cas(it);
        if ((cas_emit == 0) ||
            (cas_emit < 0 &&
             cas == CPROXY_NOT_CAS)) {
            if (add_conn_item(uc, it)) {
                it->refcount++;

                if (add_iov(uc, "VALUE ", 6) == 0 &&
                    add_iov(uc, ITEM_key(it), it->nkey) == 0 &&
                    add_iov(uc, ITEM_suffix(it),
                            it->nsuffix + it->nbytes) == 0) {
                    if (settings.verbose > 2) {
                        moxi_log_write("<%d cproxy ascii item response success\n",
                                       uc->sfd);
                    }
                }
            }
        } else {
            char *suffix = add_conn_suffix(uc);
            if (suffix != NULL) {
                sprintf(suffix, " %llu\r\n", (unsigned long long) cas);

                if (add_conn_item(uc, it)) {
                    it->refcount++;

                    if (add_iov(uc, "VALUE ", 6) == 0 &&
                        add_iov(uc, ITEM_key(it), it->nkey) == 0 &&
                        add_iov(uc, ITEM_suffix(it),
                                it->nsuffix - 2) == 0 &&
                        add_iov(uc, suffix, strlen(suffix)) == 0 &&
                        add_iov(uc, ITEM_data(it), it->nbytes) == 0) {
                        if (settings.verbose > 2) {
                            moxi_log_write("<%d cproxy ascii item response ok\n",
                                    uc->sfd);
                        }
                    }
                }
            }
        }
    } else {
        if (settings.verbose > 1) {
            moxi_log_write("ERROR: unexpected downstream data block");
        }
    }
}

/**
 * When we're sending an ascii response line back upstream to
 * an ascii protocol client, keep the front_cache sync'ed.
 */
void cproxy_del_front_cache_key_ascii_response(downstream *d,
                                               char *response,
                                               char *command) {
    assert(d);
    assert(d->ptd);
    assert(d->ptd->proxy);
    assert(response);

    if (!mcache_started(&d->ptd->proxy->front_cache)) {
        return;
    }

    // TODO: Not sure if we need all these checks, or just
    // clear the cache item no matter what.
    //
    if (strncmp(response, "DELETED", 7) == 0 ||
        strncmp(response, "STORED", 6) == 0 ||
        strncmp(response, "EXISTS", 6) == 0 ||
        strncmp(response, "NOT_FOUND", 9) == 0 ||
        strncmp(response, "NOT_STORED", 10) == 0 ||
        strncmp(response, "ERROR", 5) == 0 ||
        strncmp(response, "SERVER_ERROR", 12) == 0 ||
        (response[0] == '-') ||
        (response[0] >= '0' && response[0] <= '9')) {
        cproxy_del_front_cache_key_ascii(d, command);
    }
}

void cproxy_del_front_cache_key_ascii(downstream *d,
                                      char *command) {
    assert(d);
    assert(d->ptd);
    assert(d->ptd->proxy);

    if (d->ptd->behavior_pool.base.front_cache_lifespan == 0) {
        return;
    }

    if (mcache_started(&d->ptd->proxy->front_cache)) {
        char *spc = strchr(command, ' ');
        if (spc != NULL) {
            char *key = spc + 1;
            int   key_len = skey_len(key);

            cproxy_front_cache_delete(d->ptd, key, key_len);
        }
    }
}

/**
 * Depending on our configuration, we can optimize SET's
 * on certain keys by making them fire-and-forget and
 * immediately transmitting a success response to the
 * upstream client.
 */
bool cproxy_optimize_set_ascii(downstream *d, conn *uc,
                               char *key, int key_len) {
    assert(d);
    assert(d->ptd);
    assert(d->ptd->proxy);
    assert(uc);
    assert(uc->next == NULL);

    if (d->ptd->behavior_pool.base.optimize_set[0] == '\0') {
        return false;
    }

    if (matcher_check(&d->ptd->proxy->optimize_set_matcher,
                      key, key_len, false)) {
        d->upstream_conn = NULL;
        d->upstream_suffix = NULL;
        d->upstream_suffix_len = 0;
        d->upstream_status = PROTOCOL_BINARY_RESPONSE_SUCCESS;
        d->upstream_retry = 0;
        d->target_host_ident = NULL;

        out_string(uc, "STORED");

        if (!update_event(uc, EV_WRITE | EV_PERSIST)) {
            if (settings.verbose > 1) {
                moxi_log_write("ERROR: Can't update upstream write event\n");
            }

            d->ptd->stats.stats.err_oom++;
            cproxy_close_conn(uc);
        }

        return true;
    }

    return false;
}

void cproxy_process_downstream_ascii(conn *c, char *line) {
    downstream *d = c->extra;
    assert(d != NULL);
    assert(d->upstream_conn != NULL);

    if (IS_ASCII(d->upstream_conn->protocol)) {
        cproxy_process_a2a_downstream(c, line);
    } else {
        assert(false); // TODO: b2a.
    }
}

void cproxy_process_downstream_ascii_nread(conn *c) {
    downstream *d = c->extra;
    assert(d != NULL);
    assert(d->upstream_conn != NULL);

    if (IS_ASCII(d->upstream_conn->protocol)) {
        cproxy_process_a2a_downstream_nread(c);
    } else {
        assert(false); // TODO: b2a.
    }
}

bool cproxy_is_broadcast_cmd(int cmd) {
    return (cmd == PROTOCOL_BINARY_CMD_FLUSH ||
            cmd == PROTOCOL_BINARY_CMD_STAT || // In a2x translation.
            cmd == PROTOCOL_BINARY_CMD_NOOP ||
            cmd == PROTOCOL_BINARY_CMD_GETKQ);
}

bool ascii_scan_key(char *line, char **key, int *key_len) {
    char *curr = line;

    while (*curr != '\0' &&
           *curr == ' ') { // Scan to start of cmd.
        curr++;
    }

    while (*curr != '\0' &&
           *curr != ' ') { // Scan to end of cmd.
        curr++;
    }

    while (*curr != '\0' &&
           *curr == ' ') { // Scan to start of key.
        curr++;
    }

    *key = curr;

    while (*curr != '\0' &&
           *curr != ' ') { // Scan to end of key.
        curr++;
    }

    *key_len = (int) (curr - *key);

    return *key_len > 0;
}

void cproxy_ascii_broadcast_suffix(downstream *d) {
    conn *uc = d->upstream_conn;
    if (uc != NULL &&
        uc->noreply == false) {
        if (uc->cmd_curr == PROTOCOL_BINARY_CMD_FLUSH) {
            d->upstream_suffix = "OK\r\n";
        } else {
            d->upstream_suffix = "END\r\n";
        }

        d->upstream_suffix_len = 0;
        d->upstream_status = PROTOCOL_BINARY_RESPONSE_SUCCESS;
        d->upstream_retry = 0;
        d->target_host_ident = NULL;
    }
}
