/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#ifndef AGENT_H
#define AGENT_H

#ifdef HAVE_CONFLATE_H
#include <libconflate/conflate.h>
#endif
#ifdef REDIRECTS_FOR_MOCKS
#include "redirects.h"
#endif

int cproxy_init_agent(char *cfg_str,
                      proxy_behavior behavior,
                      int nthreads);

proxy_main *cproxy_init_agent_start(char *jid, char *jpw,
                                    char *config, char *host,
                                    proxy_behavior behavior,
                                    int nthreads);

#ifdef HAVE_CONFLATE_H
conflate_result on_conflate_new_config(void *userdata, kvpair_t *config);

enum conflate_mgmt_cb_result on_conflate_get_stats(void *opaque,
                                                   conflate_handle_t *handle,
                                                   const char *cmd,
                                                   bool direct,
                                                   kvpair_t *form,
                                                   conflate_form_result *);
enum conflate_mgmt_cb_result on_conflate_reset_stats(void *opaque,
                                                     conflate_handle_t *handle,
                                                     const char *cmd,
                                                     bool direct,
                                                     kvpair_t *form,
                                                     conflate_form_result *);
enum conflate_mgmt_cb_result on_conflate_ping_test(void *opaque,
                                                   conflate_handle_t *handle,
                                                   const char *cmd,
                                                   bool direct,
                                                   kvpair_t *form,
                                                   conflate_form_result *);
#endif

void cproxy_on_config_pool(proxy_main *m,
                           char *name, int port,
                           char *config_str,
                           uint32_t config_ver,
                           proxy_behavior_pool *behavior_pool);

#ifdef HAVE_CONFLATE_H
char **get_key_values(kvpair_t *kvs, char *key);
#endif

void proxy_stats_dump_basic(ADD_STAT add_stats, conn *c,
                            const char *prefix);
void proxy_stats_dump_proxy_main(ADD_STAT add_stats, conn *c,
                                 struct proxy_stats_cmd_info *pscip);
void proxy_stats_dump_proxies(ADD_STAT add_stats, conn *c,
                              struct proxy_stats_cmd_info *pscip);
void proxy_stats_dump_timings(ADD_STAT add_stats, conn *c);
void proxy_stats_dump_config(ADD_STAT add_stats, conn *c);

void proxy_stats_reset(proxy_main *m);

#endif /* AGENT_H */
