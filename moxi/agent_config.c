/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <assert.h>
#include "memcached.h"
#include "cproxy.h"
#include "work.h"
#include "agent.h"
#include "log.h"
#include "cJSON.h"

// Integration with libconflate.
//
static void update_ptd_config(void *data0, void *data1);
static bool update_str_config(char **curr, char *next, char *descrip);

static bool update_behaviors_config(proxy_behavior **curr,
                                    int  *curr_num,
                                    proxy_behavior  *next,
                                    int   next_num,
                                    char *descrip);

void close_outdated_proxies(proxy_main *m, uint32_t new_config_ver);

char *parse_kvs_servers(char *prefix,
                        char *pool_name,
                        kvpair_t *kvs,
                        char **servers,
                        proxy_behavior_pool *behavior_pool);

char **parse_kvs_behavior(kvpair_t *kvs,
                          char *prefix,
                          char *name,
                          proxy_behavior *behavior);

static void cproxy_parse_json_auth(char *config,
                                   char *name,
                                   proxy_behavior_pool *bp);

static void cproxy_init_null_bucket(proxy_main *m);

static void cproxy_on_config(void *data0, void *data1);

static void agent_logger(void *userdata,
                         enum conflate_log_level lvl,
                         const char *msg, ...)
{
    (void)userdata;
    (void)lvl;
    (void)msg;
// Issues compiling vfprintf(), so turn off this unused code path for now.
#undef AGENT_LOGGER
#ifdef AGENT_LOGGER
    char *n = NULL;
    bool v = false;

    switch(lvl) {
    case FATAL: n = "FATAL"; v = settings.verbose > 0; break;
    case ERROR: n = "ERROR"; v = settings.verbose > 0; break;
    case WARN:  n = "WARN";  v = settings.verbose > 1; break;
    case INFO:  n = "INFO";  v = settings.verbose > 1; break;
    case DEBUG: n = "DEBUG"; v = settings.verbose > 2; break;
    }
    if (!v) {
        return;
    }

    char fmt[strlen(msg) + 16];
    snprintf(fmt, sizeof(fmt), "%s: %s\n", n, msg);

    va_list ap;
    va_start(ap, msg);
    vfprintf(fmt, ap);
    va_end(ap);
#endif
}

static void init_extensions(void)
{
    conflate_register_mgmt_cb("client_stats", "Retrieve stats from moxi",
                              on_conflate_get_stats);
    conflate_register_mgmt_cb("reset_stats", "Reset moxi stats",
                              on_conflate_reset_stats);
    conflate_register_mgmt_cb("ping_test", "Perform a ping test",
                              on_conflate_ping_test);
}

/** The cfg_str looks like...
 *
 *    apikey=jidname@jhostname%jpassword,config=config,host=host
 *      or...
 *    jidname@jhostname%jpassword,config=config,host=host
 *
 *  Only the apikey is needed, so it can also look like...
 *
 *    jidname@jhostname%jpassword
 *
 *  Or...
 *
 *    http://host:port/default/pools/bucketsStreamingConfig/default
 *    url=http://host:port/default/pools/bucketsStreamingConfig/default
 *    auth=,url=http://host:port/default/pools/bucketsStreamingConfig/default
 *    auth=USER%PSWD,url=http://host:port/default/pools/bucketsStreamingConfig/default
 *    auth=Administrator%password,url=http://host:port/default/pools/bucketsStreamingConfig/default
 */
int cproxy_init_agent(char *cfg_str,
                      proxy_behavior behavior,
                      int nthreads) {
    init_extensions();

    if (cfg_str == NULL) {
        moxi_log_write("ERROR: missing cfg\n");
        if (ml->log_mode != ERRORLOG_STDERR) {
            fprintf(stderr, "ERROR: missing cfg\n");
        }
        exit(EXIT_FAILURE);
    }

    int cfg_len = strlen(cfg_str);
    if (cfg_len <= 0) {
        moxi_log_write("ERROR: empty cfg\n");
        if (ml->log_mode != ERRORLOG_STDERR) {
            fprintf(stderr, "ERROR: empty cfg\n");
        }
        exit(EXIT_FAILURE);
    }

    char *buff;

    if (strncmp(cfg_str, "apikey=", 7) == 0 ||
        strncmp(cfg_str, "auth=", 5) == 0 ||
        strncmp(cfg_str, "url=", 4) == 0) {
        buff = trimstrdup(cfg_str);
    } else {
        buff = calloc(cfg_len + 50, sizeof(char));
        if (buff != NULL) {
            if (strncmp(cfg_str, "http://", 7) == 0) {
                snprintf(buff, cfg_len + 50, "url=%s", cfg_str);

                // Allow the user to specify multiple comma-separated URL's,
                // which we auto-translate right now to the '|' separators
                // that the rest of the code expects.
                //
                for (char *x = buff; *x; x++) {
                    if (*x == ',') {
                        *x = '|';
                    }
                }
            } else {
                strcpy(buff, cfg_str);
            }
        }
        buff = trimstr(buff);
    }

    char *next = buff;

    int rv = 0;

    while (next != NULL) {
        char *jid    = behavior.usr;
        char *jpw    = behavior.pwd;
        char *jpwmem = NULL;
        char *dbpath = NULL;
        char *host   = NULL;

        char *cur = trimstr(strsep(&next, ";"));
        while (cur != NULL) {
            char *key_val = trimstr(strsep(&cur, ",\r\n"));
            if (key_val != NULL) {
                char *key = trimstr(strsep(&key_val, "="));
                char *val = trimstr(key_val);

                bool handled = true;

                if (key != NULL &&
                    val != NULL) {
                    if (wordeq(key, "apikey") ||
                        wordeq(key, "auth")) {
                        jid = strsep(&val, "%");
                        jpw = val;
                    } else if (wordeq(key, "config") ||
                               wordeq(key, "dbpath")) {
                        dbpath = val;
                    } else if (wordeq(key, "host") ||
                               wordeq(key, "url")) {
                        host = val;
                    } else {
                        handled = false;
                    }
                } else {
                    handled = false;
                }

                if (handled == false &&
                    key != NULL &&
                    key[0] != '#' &&
                    key[0] != '\0') {
                    if (settings.verbose > 0) {
                        moxi_log_write("unknown configuration key: %s\n", key);
                    }
                }
            }
        }

        if (jid == NULL) {
            jid = "";
        }

        if (jpw == NULL) {
            // Handle if jid/jpw is in user:password@fqdn format
            // instead of user@fqdn%password format.
            //
            char *colon = strchr(jid, ':');
            char *asign = strchr(jid, '@');
            if (colon != NULL &&
                asign != NULL &&
                asign > colon) {
                *asign = '\0';
                jpw = jpwmem = strdup(colon + 1);
                *asign = '@';
                do {
                    *colon = *asign;
                    colon++;
                    asign++;
                } while (*asign != '\0');
                *colon = '\0';
            }
        }

        if (jpw == NULL) {
            jpw = "";
        }

        int dbpath_alloc = 0;
        if (dbpath == NULL) {
            dbpath_alloc = strlen(jid) + strlen(CONFLATE_DB_PATH) + 100;
            dbpath = calloc(dbpath_alloc, 1);
            if (dbpath != NULL) {
                snprintf(dbpath, dbpath_alloc,
                         CONFLATE_DB_PATH "/conflate-%s.cfg",
                         (jid != NULL && strlen(jid) > 0 ? jid : "default"));

            } else {
                moxi_log_write("ERROR: conflate dbpath buf alloc\n");
                exit(EXIT_FAILURE);
            }
        }

        if (settings.verbose > 1) {
            moxi_log_write("cproxy_init jid: %s host: %s\n", jid, host);
        }

        if (cproxy_init_agent_start(jid, jpw, dbpath, host,
                                    behavior,
                                    nthreads) != NULL) {
            rv++;
        }

        if (dbpath_alloc > 0 &&
            dbpath != NULL) {
            free(dbpath);
        }

        if (jpwmem) {
            free(jpwmem);
        }
    }

    free(buff);

    return rv;
}

proxy_main *cproxy_init_agent_start(char *jid,
                                    char *jpw,
                                    char *dbpath,
                                    char *host,
                                    proxy_behavior behavior,
                                    int nthreads) {
    assert(dbpath);

    if (settings.verbose > 2) {
        moxi_log_write("cproxy_init_agent_start\n");;
    }

    proxy_main *m = cproxy_gen_proxy_main(behavior, nthreads,
                                          PROXY_CONF_TYPE_DYNAMIC);
    if (m != NULL) {
        // Create a NULL_BUCKET when we're not in "FIRST_BUCKET" mode.
        //
        // FIRST_BUCKET mode means clients start off in the first
        // configured bucket (and this is usually the case for
        // standalone moxi).
        //
        // Otherwise (when not in FIRST_BUCKET mode)...
        // -- new clients start off in a configured, named default
        // bucket (whose name is usually configured to be "default"),
        // if it exists.
        // -- if the named default bucket doesn't exist, new
        // clients then start off in the NULL_BUCKET.
        //
        if (strcmp(behavior.default_bucket_name, FIRST_BUCKET) != 0) {
            if (settings.verbose > 2) {
                moxi_log_write("initializing null bucket, default is: %s\n",
                               behavior.default_bucket_name);
            }

            cproxy_init_null_bucket(m);
        } else {
            if (settings.verbose > 2) {
                moxi_log_write("using first bucket\n");
            }
        }

        conflate_config_t config;

        memset(&config, 0, sizeof(config));

        init_conflate(&config);

        // Different jid's possible for production, staging, etc.
        config.jid  = jid;  // "customer@stevenmb.local" or
                            // "Administrator"
        config.pass = jpw;  // "password"
        config.host = host; // "localhost" or
                            // "http://x.com:8091"
                            // "http://x.com:8091/pools/default/buckets/default"
        config.software   = PACKAGE;
        config.version    = VERSION;
        config.save_path  = dbpath;
        config.userdata   = m;
        config.new_config = on_conflate_new_config;
        config.log        = agent_logger;

        if (!config.host || config.host[0] == '\0') {
            moxi_log_write("ERROR: missing -z configuration for url/host\n");
            if (ml->log_mode != ERRORLOG_STDERR) {
                fprintf(stderr, "ERROR: missing -z configuration for url/host\n");
            }
            exit(EXIT_FAILURE);
        }

        if (start_conflate(config)) {
            if (settings.verbose > 2) {
                moxi_log_write("cproxy_init_agent_start done\n");
            }

            return m;
        }

        free(m);
    }

    if (settings.verbose > 1) {
        moxi_log_write("cproxy could not start conflate\n");
    }

    return NULL;
}

static void cproxy_init_null_bucket(proxy_main *m) {
    proxy_behavior proxyb = m->behavior;

    int pool_port = proxyb.port_listen;
    int nodes_num = 0;

    if (pool_port > 0) {
        proxy_behavior_pool behavior_pool = {
            .base = proxyb,
            .num  = nodes_num,
            .arr  = calloc(nodes_num + 1, sizeof(proxy_behavior))
        };

        if (behavior_pool.arr != NULL) {
            cproxy_on_config_pool(m, NULL_BUCKET, pool_port,
                                  "", 0, &behavior_pool);
            free(behavior_pool.arr);
        }
    }
}

conflate_result on_conflate_new_config(void *userdata, kvpair_t *config) {
    assert(config != NULL);

    proxy_main *m = userdata;
    assert(m != NULL);

    LIBEVENT_THREAD *mthread = thread_by_index(0);
    assert(mthread != NULL);

    if (settings.verbose > 0) {
        moxi_log_write("configuration received\n");
    }

    char **urlv = get_key_values(config, "url"); // NULL delimited array of char *.
    char  *url  = urlv != NULL ? urlv[0] : NULL;

    char **contentsv = get_key_values(config, "contents");
    char  *contents  = contentsv != NULL ? contentsv[0] : NULL;

    if (url != NULL &&
        contents != NULL &&
        strlen(contents) > 0) {
        // Must be a REST/JSON config.  Wastefully test parse it here,
        // before we asynchronously invoke the real worker who can't
        // respond nicely with an error code.
        //
        bool ok = false;

        cJSON *c = cJSON_Parse(contents);
        if (c != NULL) {
            ok = true;
            cJSON_Delete(c);
        }

        if (!ok) {
            moxi_log_write("ERROR: parse JSON failed, from REST server: %s, %s\n",
                           url, contents);

            return CONFLATE_ERROR_BAD_SOURCE;
        }
    }

    kvpair_t *copy = dup_kvpair(config);
    if (copy != NULL) {
        if (work_send(mthread->work_queue, cproxy_on_config, m, copy)) {
            return CONFLATE_SUCCESS;
        }

        if (settings.verbose > 1) {
            moxi_log_write("work_send failed\n");
        }

        return CONFLATE_ERROR;
    }

    if (settings.verbose > 1) {
        moxi_log_write("agent_config ocnc failed dup_kvpair\n");
    }

    return CONFLATE_ERROR;
}

#ifdef MOXI_USE_LIBVBUCKET

static bool cproxy_on_config_json_one(proxy_main *m, uint32_t new_config_ver,
                                      char *config, char *name, char *src);

static bool cproxy_on_config_json_one_vbucket(proxy_main *m,
                                              uint32_t new_config_ver,
                                              char *config,
                                              char *name,
                                              char *src);

static bool cproxy_on_config_json_one_ketama(proxy_main *m,
                                             uint32_t new_config_ver,
                                             char *config,
                                             char *name,
                                             char *src);

static bool cproxy_on_config_json_buckets(proxy_main *m, uint32_t new_config_ver,
                                          cJSON *jBuckets, bool want_default,
                                          char *src);

static
bool cproxy_on_config_json(proxy_main *m, uint32_t new_config_ver, char *config,
                           char *src) {
    bool rv = false;

    cJSON *c = cJSON_Parse(config);
    if (c != NULL) {
        cJSON *jBuckets = cJSON_GetObjectItem(c, "buckets");
        if (jBuckets != NULL &&
            jBuckets->type == cJSON_Array) {
            // Make two passes through jBuckets, favoring any "default"
            // bucket on the 1st pass, so the default bucket gets
            // created earlier.
            //
            bool rv1 = cproxy_on_config_json_buckets(m, new_config_ver, jBuckets,
                                                     true, src);
            bool rv2 = cproxy_on_config_json_buckets(m, new_config_ver, jBuckets,
                                                     false, src);

            rv = rv1 || rv2;
        } else {
            // Just a single config.
            //
            rv = cproxy_on_config_json_one(m, new_config_ver, config, "default", src);
        }

        cJSON_Delete(c);
    } else {
        moxi_log_write("ERROR: could not parse JSON from REST server: %s, %s\n",
                       src, config);
    }

    return rv;
}

static
bool cproxy_on_config_json_buckets(proxy_main *m, uint32_t new_config_ver,
                                   cJSON *jBuckets, bool want_default,
                                   char *src) {
    bool rv = false;

    int numBuckets = cJSON_GetArraySize(jBuckets);
    for (int i = 0; i < numBuckets; i++) {
        cJSON *jBucket = cJSON_GetArrayItem(jBuckets, i);
        if (jBucket != NULL &&
            jBucket->type == cJSON_Object) {
            char *name = "default";

            cJSON *jName = cJSON_GetObjectItem(jBucket, "name");
            if (jName != NULL &&
                jName->type == cJSON_String &&
                jName->valuestring != NULL) {
                name = jName->valuestring;
            }

            bool is_default = (strcmp(name, "default") == 0);
            if (!(is_default ^ want_default)) { // XOR.
                char *jBucketStr = cJSON_Print(jBucket);
                if (jBucketStr != NULL) {
                    rv = cproxy_on_config_json_one(m, new_config_ver,
                                                   jBucketStr, name, src) || rv;
                    free(jBucketStr);
                }
            }
        }
    }

    return rv;
}

static
bool cproxy_on_config_json_one(proxy_main *m, uint32_t new_config_ver,
                               char *config, char *name,
                               char *src) {
    assert(m != NULL);
    assert(config != NULL);
    assert(name != NULL);

    bool rv = false;

    // Handle reconfiguration of a single proxy.
    //
    if (m != NULL &&
        config != NULL &&
        strlen(config) > 0) {
        if (settings.verbose > 2) {
            moxi_log_write("conjo contents config from %s: %s\n", src, config);
        }

        // The config should be JSON that should look like...
        //
        // {"name":"default",                // The bucket name.
        //  "nodeLocator":"ketama",          // Optional.
        //  "saslPassword":"someSASLPwd",
        //  "nodes":[{"hostname":"10.17.1.46","status":"healthy",
        //            "version":"0.3.0_114_g31859fe","os":"i386-apple-darwin9.8.0",
        //            "ports":{"proxy":11213,"direct":11212}}],
        //  "buckets":{"uri":"/pools/default/buckets"},
        //  "controllers":{"ejectNode":{"uri":"/controller/ejectNode"},
        //  "testWorkload":{"uri":"/pools/default/controller/testWorkload"}},
        //  "stats":{"uri":"/pools/default/stats"},
        //  "vBucketServerMap":{
        //     "hashAlgorithm":"CRC",
        //     "user":"optionalSASLUsr",     // Optional.
        //     "password":"someSASLPwd",     // Optional.
        //     "serverList":["10.17.1.46:11212"],
        //     ...more json here...}}
        //
        cJSON *jConfig = cJSON_Parse(config);
        if (jConfig != NULL) {
            cJSON *jName = cJSON_GetObjectItem(jConfig, "name");
            if (jName != NULL &&
                jName->type == cJSON_String &&
                jName->valuestring != NULL) {
                name = jName->valuestring;
            }

            cJSON *jNodeLocator = cJSON_GetObjectItem(jConfig, "nodeLocator");
            if (jNodeLocator != NULL &&
                jNodeLocator->type == cJSON_String &&
                jNodeLocator->valuestring != NULL) {
                if (strcmp(jNodeLocator->valuestring, "ketama") == 0) {
                    rv = cproxy_on_config_json_one_ketama(m, new_config_ver,
                                                          config, name, src);
                    cJSON_Delete(jConfig);

                    return rv;
                }
            }

            rv = cproxy_on_config_json_one_vbucket(m, new_config_ver,
                                                   config, name, src);
            cJSON_Delete(jConfig);
        }
    } else {
        if (settings.verbose > 1) {
            moxi_log_write("ERROR: skipping empty config from %s\n", src);
        }
    }

    return rv;
}

static
bool cproxy_on_config_json_one_vbucket(proxy_main *m, uint32_t new_config_ver,
                                       char *config, char *name,
                                       char *src) {
    assert(m != NULL);

    bool rv = false;

    if (settings.verbose > 2) {
        moxi_log_write("parsing config nodeLocator:vbucket\n");
    }

    VBUCKET_CONFIG_HANDLE vch = vbucket_config_parse_string(config);
    if (vch) {
        if (settings.verbose > 2) {
            moxi_log_write("conc vbucket_config_parse_string: %d for %s\n",
                           (vch != NULL), name);
        }

        proxy_behavior proxyb = m->behavior;
        strcpy(proxyb.nodeLocator, "vbucket");

        int pool_port = proxyb.port_listen;
        int nodes_num = vbucket_config_get_num_servers(vch);

        if (settings.verbose > 2) {
            moxi_log_write("conc pool_port: %d nodes_num: %d\n",
                           pool_port, nodes_num);
        }

        if (pool_port > 0 &&
            nodes_num > 0) {
            proxy_behavior_pool behavior_pool = {
                .base = proxyb,
                .num  = nodes_num,
                .arr  = calloc(nodes_num, sizeof(proxy_behavior))
            };

            if (behavior_pool.arr != NULL) {
                cproxy_parse_json_auth(config, name, &behavior_pool);

                int j = 0;
                for (; j < nodes_num; j++) {
                    // Inherit default behavior.
                    //
                    behavior_pool.arr[j] = behavior_pool.base;

                    const char *hostport = vbucket_config_get_server(vch, j);
                    if (hostport != NULL &&
                        strlen(hostport) > 0 &&
                        strlen(hostport) < sizeof(behavior_pool.arr[j].host) - 1) {
                        strncpy(behavior_pool.arr[j].host,
                                hostport,
                                sizeof(behavior_pool.arr[j].host) - 1);
                        behavior_pool.arr[j].host[sizeof(behavior_pool.arr[j].host) - 1] = '\0';

                        char *colon = strchr(behavior_pool.arr[j].host, ':');
                        if (colon != NULL) {
                            *colon = '\0';
                            behavior_pool.arr[j].port = atoi(colon + 1);
                            if (behavior_pool.arr[j].port <= 0) {
                                break;
                            }
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }

                if (j >= nodes_num) {
                    cproxy_on_config_pool(m, name, pool_port,
                                          config, new_config_ver,
                                          &behavior_pool);
                    rv = true;
                } else {
                    if (settings.verbose > 1) {
                        moxi_log_write("ERROR: error receiving host:port"
                                       " from %s"
                                       " for server config %d in %s\n",
                                       src, j, config);
                    }
                }

                free(behavior_pool.arr);
            }
        }

        vbucket_config_destroy(vch);
    } else {
        moxi_log_write("ERROR: bad JSON configuration from %s: %s (%s)\n",
                       src, vbucket_get_error(), config);
        if (ml->log_mode != ERRORLOG_STDERR) {
            fprintf(stderr, "ERROR: bad JSON configuration from %s: %s (%s)\n",
                    src, vbucket_get_error(), config);
        }

        // Bug 1961 - don't exit() as we might be in a multitenant use case.
        //
        // exit(EXIT_FAILURE);
    }

    return rv;
}

static
bool cproxy_on_config_json_one_ketama(proxy_main *m, uint32_t new_config_ver,
                                      char *config, char *name,
                                      char *src) {
    assert(m != NULL);

    bool rv = false;

#ifdef MOXI_USE_LIBMEMCACHED

    if (settings.verbose > 2) {
        moxi_log_write("parsing config nodeLocator:ketama\n");
    }

    // First, try to iterate through jConfig.vBucketServerMap.serverList
    // if it exists, otherwise iterate through jConfig.nodes.
    //
    cJSON *jConfig = cJSON_Parse(config);
    if (jConfig == NULL) {
        return false;
    }

    cJSON *jArr = NULL;

    cJSON *jVBSM = cJSON_GetObjectItem(jConfig, "vBucketServerMap");
    if (jVBSM != NULL) {
        jArr = cJSON_GetObjectItem(jVBSM, "serverList");
    }

    if (jArr == NULL ||
        jArr->type != cJSON_Array) {
        jArr = cJSON_GetObjectItem(jConfig, "nodes");
    }

    if (jArr != NULL &&
        jArr->type == cJSON_Array) {
        int nodes_num = cJSON_GetArraySize(jArr);
        if (nodes_num > 0) {
            proxy_behavior proxyb = m->behavior;
            strcpy(proxyb.nodeLocator, "ketama");

            if (settings.verbose > 2) {
                moxi_log_write("conjk nodes_num: %d\n", nodes_num);
            }

            proxy_behavior_pool behavior_pool = {
                .base = proxyb,
                .num  = nodes_num,
                .arr  = calloc(nodes_num + 1, sizeof(proxy_behavior))
            };

            if (behavior_pool.arr != NULL) {
                cproxy_parse_json_auth(config, name, &behavior_pool);

                int curr = 0; // Moves slower than j so we can skip unhealthy nodes.

                int j = 0;
                for (; j < nodes_num; j++) {
                    // Inherit default behavior.
                    //
                    behavior_pool.arr[curr] = behavior_pool.base;

                    cJSON *jNode = cJSON_GetArrayItem(jArr, j);
                    if (jNode != NULL) {
                        if (jNode->type == cJSON_String &&
                            jNode->valuestring != NULL) {
                            // Should look like "host:port".
                            //
                            char *hostport = jNode->valuestring;

                            if (strlen(hostport) > 0 &&
                                strlen(hostport) < sizeof(behavior_pool.arr[curr].host) - 1) {
                                strncpy(behavior_pool.arr[curr].host,
                                        hostport,
                                        sizeof(behavior_pool.arr[curr].host) - 1);
                                behavior_pool.arr[curr].host[sizeof(behavior_pool.arr[curr].host) - 1] = '\0';

                                char *colon = strchr(behavior_pool.arr[curr].host, ':');
                                if (colon != NULL) {
                                    *colon = '\0';
                                    behavior_pool.arr[curr].port = atoi(colon + 1);
                                    if (behavior_pool.arr[curr].port > 0) {
                                        curr++;
                                    } else {
                                        break;
                                    }
                                } else {
                                    break;
                                }
                            } else {
                                break;
                            }
                        } else if (jNode->type == cJSON_Object) {
                            // Should look like... {
                            //   status: "healthy",
                            //   hostname: "host",
                            //   ports: { direct: port }
                            // }
                            //
                            cJSON *jStatus = cJSON_GetObjectItem(jNode, "status");
                            if (jStatus != NULL &&
                                jStatus->type == cJSON_String &&
                                jStatus->valuestring != NULL &&
                                strcmp(jStatus->valuestring, "healthy") != 0) {
                                // Skip non-healthy node.
                                //
                                continue;
                            }

                            cJSON *jHostname = cJSON_GetObjectItem(jNode, "hostname");
                            if (jHostname != NULL &&
                                jHostname->type == cJSON_String &&
                                jHostname->valuestring != NULL &&
                                strlen(jHostname->valuestring) < sizeof(behavior_pool.arr[curr].host) - 1) {
                                cJSON *jPorts = cJSON_GetObjectItem(jNode, "ports");
                                if (jPorts != NULL &&
                                    jPorts->type == cJSON_Object) {
                                    cJSON *jDirect = cJSON_GetObjectItem(jPorts, "direct");
                                    if (jDirect != NULL &&
                                        jDirect->type == cJSON_Number &&
                                        jDirect->valueint > 0) {
                                        strncpy(behavior_pool.arr[curr].host,
                                                jHostname->valuestring,
                                                sizeof(behavior_pool.arr[curr].host) - 1);
                                        behavior_pool.arr[curr].host[sizeof(behavior_pool.arr[curr].host) - 1] = '\0';

                                        // The JSON might return a hostname that looks
                                        // like "HOST:REST_PORT", so strip off the ":REST_PORT".
                                        //
                                        char *colon = strchr(behavior_pool.arr[curr].host, ':');
                                        if (colon != NULL) {
                                            *colon = '\0';
                                        }

                                        behavior_pool.arr[curr].port = jDirect->valueint;

                                        curr++;
                                    } else {
                                        break;
                                    }
                                } else {
                                    break;
                                }
                            } else {
                                break;
                            }
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }

                if (j >= nodes_num && curr > 0) {
                    // Some unhealthy nodes might have been skipped,
                    // so curr might be <= behavior_pool.num.
                    //
                    behavior_pool.num = curr;

                    // Create a config string that libmemcached likes,
                    // such as "HOST:PORT,HOST:PORT,HOST:PORT".
                    //
                    int   config_len = 200;
                    char *config_str = calloc(config_len, 1);

                    if (config_str != NULL) {
                        for (j = 0; j < behavior_pool.num; j++) {
                            // Grow config string for libmemcached.
                            //
                            int x = 40 + // For port and weight.
                                strlen(config_str) +
                                strlen(behavior_pool.arr[j].host);
                            if (config_len < x) {
                                config_len = 2 * (config_len + x);
                                config_str = realloc(config_str, config_len);
                                if (config_str == NULL) {
                                    break;
                                }
                            }

                            char *config_end = config_str + strlen(config_str);
                            if (config_end != config_str) {
                                *config_end++ = ',';
                            }

                            if (strlen(behavior_pool.arr[j].host) > 0 &&
                                behavior_pool.arr[j].port > 0) {
                                snprintf(config_end,
                                         config_len - (config_end - config_str),
                                         "%s:%u",
                                         behavior_pool.arr[j].host,
                                         behavior_pool.arr[j].port);
                            } else {
                                if (settings.verbose > 1) {
                                    moxi_log_write("ERROR: conjk missing host/port %d in %s from %s\n",
                                                   j, name, src);
                                }
                            }

                            if (behavior_pool.arr[j].downstream_weight > 0) {
                                config_end = config_str + strlen(config_str);
                                snprintf(config_end,
                                         config_len - (config_end - config_str),
                                         ":%u",
                                         behavior_pool.arr[j].downstream_weight);
                            }
                        }

                        if (config_str != NULL) {
                            if (j >= behavior_pool.num) {
                                cproxy_on_config_pool(m, name, proxyb.port_listen,
                                                      config_str, new_config_ver,
                                                      &behavior_pool);
                                rv = true;
                            }

                            free(config_str);
                        }
                    } else {
                        if (settings.verbose > 1) {
                            moxi_log_write("ERROR: oom on jk re-config str\n");;
                        }
                    }
                } else {
                    if (settings.verbose > 1) {
                        moxi_log_write("ERROR: conjk parse error for config %d from %s in %s\n",
                                       j, src, config);
                    }
                }

                free(behavior_pool.arr);
            } else {
                if (settings.verbose > 1) {
                    moxi_log_write("ERROR: oom on jk re-config\n");;
                }
            }
        } else {
            if (settings.verbose > 1) {
                moxi_log_write("ERROR: conjk empty serverList/nodes in re-config\n");;
            }
        }
    } else {
        if (settings.verbose > 1) {
            moxi_log_write("ERROR: conjk no serverList/nodes in re-config\n");;
        }
    }

    cJSON_Delete(jConfig);

#else // !MOXI_USE_LIBMEMCACHED

    (void) m;
    (void) new_config_ver;
    (void) config;
    (void) name;

    moxi_log_write("ERROR: not compiled with libmemcached support\n");

#endif // !MOXI_USE_LIBMEMCACHED

    return rv;
}

static void cproxy_parse_json_auth(char *config,
                                   char *name,
                                   proxy_behavior_pool *bp) {
    strncpy(bp->base.usr, name, sizeof(bp->base.usr) - 1);
    bp->base.usr[sizeof(bp->base.usr) - 1] = '\0';

    cJSON *jConfig = cJSON_Parse(config);
    if (jConfig != NULL) {
        cJSON *jPassword = cJSON_GetObjectItem(jConfig, "saslPassword");
        if (jPassword != NULL &&
            jPassword->type == cJSON_String &&
            jPassword->valuestring != NULL) {
            strncpy(bp->base.pwd,
                    jPassword->valuestring,
                    sizeof(bp->base.pwd) - 1);
            bp->base.pwd[sizeof(bp->base.pwd) - 1] = '\0';
        }

        cJSON_Delete(jConfig);
    }
}

#else // !MOXI_USE_LIBVBUCKET

static
bool cproxy_on_config_kvs(proxy_main *m, uint32_t new_config_ver, kvpair_t *kvs) {
    // The kvs key-multivalues look roughly like...
    //
    //  pool-customer1-a
    //    svrname3
    //  pool-customer1-b
    //    svrname1
    //    svrname2
    //  svr-svrname1
    //    host=mc1.foo.net
    //    port=11211
    //    weight=1
    //    bucket=buck1
    //    usr=test1
    //    pwd=password
    //  svr-svrnameX
    //    host=mc2.foo.net
    //    port=11211
    //  behavior-customer1-a
    //    wait_queue_timeout=1000
    //    downstream_max=10
    //  behavior-customer1-b
    //    wait_queue_timeout=1000
    //    downstream_max=10
    //  pool_drain-customer1-b
    //    svrname1
    //    svrname3
    //  pools
    //    customer1-a
    //    customer1-b
    //  bindings
    //    11221
    //    11331
    //
    char **pools    = get_key_values(kvs, "pools");
    char **bindings = get_key_values(kvs, "bindings");

    if (pools == NULL) {
        return false;
    }

    int npools    = 0;
    int nbindings = 0;

    while (pools && pools[npools])
        npools++;

    while (bindings && bindings[nbindings])
        nbindings++;

    if (nbindings > 0 &&
        nbindings != npools) {
        if (settings.verbose > 1) {
            moxi_log_write("npools does not match nbindings\n");
        }
        return false;
    }

    char **behavior_kvs = get_key_values(kvs, "behavior");
    if (behavior_kvs != NULL) {
        // Update the default behavior.
        //
        proxy_behavior m_behavior = m->behavior;

        for (int k = 0; behavior_kvs[k]; k++) {
            char *bstr = trimstrdup(behavior_kvs[k]);
            if (bstr != NULL) {
                cproxy_parse_behavior_key_val_str(bstr, &m_behavior);
                free(bstr);
            }
        }

        m->behavior = m_behavior;
    }

    for (int i = 0; i < npools; i++) {
        char *pool_name = skipspace(pools[i]);
        if (pool_name != NULL &&
            pool_name[0] != '\0') {
            char buf[200];

            snprintf(buf, sizeof(buf), "pool-%s", pool_name);

            char **servers = get_key_values(kvs, trimstr(buf));
            if (servers != NULL) {
                // Parse proxy-level behavior.
                //
                proxy_behavior proxyb = m->behavior;

                if (parse_kvs_behavior(kvs, "behavior", pool_name, &proxyb)) {
                    if (settings.verbose > 1) {
                        cproxy_dump_behavior(&proxyb,
                                             "conc proxy_behavior", 1);
                    }
                }

                // The legacy way to get a port is through the bindings,
                // but they're also available as an inheritable
                // proxy_behavior field of port_listen.
                //
                int pool_port = proxyb.port_listen;

                if (i < nbindings &&
                    bindings != NULL &&
                    bindings[i]) {
                    pool_port = atoi(skipspace(bindings[i]));
                }

                if (pool_port > 0) {
                    // Number of servers in this pool.
                    //
                    int s = 0;
                    while (servers[s]) {
                        s++;
                    }

                    if (s > 0) {
                        // Parse server-level behaviors, so we'll have an
                        // array of behaviors, one entry for each server.
                        //
                        proxy_behavior_pool behavior_pool = {
                            .base = proxyb,
                            .num  = s,
                            .arr  = calloc(s, sizeof(proxy_behavior))
                        };

                        if (behavior_pool.arr != NULL) {
                            char *config_str =
                                parse_kvs_servers("svr", pool_name, kvs,
                                                  servers, &behavior_pool);
                            if (config_str != NULL &&
                                config_str[0] != '\0') {
                                if (settings.verbose > 2) {
                                    moxi_log_write("conc config: %s\n",
                                            config_str);
                                }

                                cproxy_on_config_pool(m, pool_name, pool_port,
                                                      config_str, new_config_ver,
                                                      &behavior_pool);

                                free(config_str);
                            }

                            free(behavior_pool.arr);
                        } else {
                            if (settings.verbose > 1) {
                                moxi_log_write("ERROR: oom on re-config malloc\n");;
                            }
                            return false;
                        }
                    } else {
                        // Note: ignore when no servers for an existing pool.
                        // Because the config_ver won't be updated, we'll
                        // fall into the empty_pool code path below.
                    }
                } else {
                    if (settings.verbose > 1) {
                        moxi_log_write("ERROR: conc missing pool port\n");
                    }
                    return false;
                }
            } else {
                // Note: ignore when no servers for an existing pool.
                // Because the config_ver won't be updated, we'll
                // fall into the empty_pool code path below.
            }
        } else {
            if (settings.verbose > 1) {
                moxi_log_write("ERROR: conc missing pool name\n");
            }
            return false;
        }
    }

    return true;
}

#endif // !MOXI_USE_LIBVBUCKET

static
void cproxy_on_config(void *data0, void *data1) {
    proxy_main *m = data0;
    assert(m);

    kvpair_t *kvs = data1;
    assert(kvs);
    assert(is_listen_thread());

    m->stat_configs++;

    uint32_t max_config_ver = 0;

    pthread_mutex_lock(&m->proxy_main_lock);

    for (proxy *p = m->proxy_head; p != NULL; p = p->next) {
        pthread_mutex_lock(&p->proxy_lock);
        if (max_config_ver < p->config_ver) {
            max_config_ver = p->config_ver;
        }
        pthread_mutex_unlock(&p->proxy_lock);
    }

    pthread_mutex_unlock(&m->proxy_main_lock);

    uint32_t new_config_ver = max_config_ver + 1;

    if (settings.verbose > 2) {
        moxi_log_write("conc new_config_ver %u\n", new_config_ver);
    }

#ifdef MOXI_USE_LIBVBUCKET
    char **urlv = get_key_values(kvs, "url"); // NULL delimited array of char *.
    char  *url  = urlv != NULL ? (urlv[0] != NULL ? urlv[0] : "") : "";

    char **contents = get_key_values(kvs, "contents");
    if (contents != NULL &&
        contents[0] != NULL) {
        char *config = trimstrdup(contents[0]);
        if (config != NULL &&
            strlen(config) > 0) {
            cproxy_on_config_json(m, new_config_ver, config, url);

            free(config);
        } else {
            moxi_log_write("ERROR: invalid, empty config from REST server %s\n",
                           url);
            goto fail;
        }
    } else {
        moxi_log_write("ERROR: invalid response from REST server %s\n",
                       url);
    }
#else // !MOXI_USE_LIBVBUCKET
    if (cproxy_on_config_kvs(m, new_config_ver, kvs) == false) {
        goto fail;
    }
#endif // !MOXI_USE_LIBVBUCKET

    // If there were any proxies that weren't updated in the
    // previous loop, we need to shut them down.  We mark the
    // proxy->config as NULL, and cproxy_check_downstream_config()
    // will catch it.
    //
    close_outdated_proxies(m, new_config_ver);

    free_kvpair(kvs);

    return;

 fail:
    m->stat_config_fails++;
    free_kvpair(kvs);

    if (settings.verbose > 1) {
        moxi_log_write("ERROR: conc failed config %llu\n",
                       (long long unsigned int) m->stat_config_fails);
    }
}

void close_outdated_proxies(proxy_main *m, uint32_t new_config_ver) {
    // TODO: Close any listening conns for the proxy?
    // TODO: Close any upstream conns for the proxy?
    // TODO: We still need to free proxy memory, after all its
    //       proxy_td's and downstreams are closed, and no more
    //       upstreams are pointed at the proxy.
    //
    proxy_behavior_pool empty_pool;
    memset(&empty_pool, 0, sizeof(proxy_behavior_pool));

    empty_pool.base = m->behavior;
    empty_pool.num  = 0;
    empty_pool.arr  = NULL;

    pthread_mutex_lock(&m->proxy_main_lock);

    for (proxy *p = m->proxy_head; p != NULL; p = p->next) {
        bool  down = false;
        int   port = 0;
        char *name = NULL;

        pthread_mutex_lock(&p->proxy_lock);

        if (p->config_ver != new_config_ver) {
            down = true;

            assert(p->port > 0);
            assert(p->name != NULL);

            port = p->port;
            name = strdup(p->name);
        }

        pthread_mutex_unlock(&p->proxy_lock);

        pthread_mutex_unlock(&m->proxy_main_lock);

        // Note, we don't want to own the proxy_main_lock here
        // because cproxy_on_config_pool() may scatter/gather
        // calls against the worker threads, and the worked threads
        // should not deadlock if they need the proxy_main_lock.
        //
        // Also, check that we're not shutting down the NULL_BUCKET.
        //
        // Otherwise, passing in a NULL config string signals that
        // a bucket's proxy struct should be shut down.
        //
        if (name != NULL) {
            if (down && (strcmp(NULL_BUCKET, name) != 0)) {
                cproxy_on_config_pool(m, name, port, NULL, new_config_ver,
                                      &empty_pool);
            }

            free(name);
        }

        pthread_mutex_lock(&m->proxy_main_lock);
    }

    pthread_mutex_unlock(&m->proxy_main_lock);
}

/**
 * A name and port uniquely identify a proxy.
 */
void cproxy_on_config_pool(proxy_main *m,
                           char *name, int port,
                           char *config,
                           uint32_t config_ver,
                           proxy_behavior_pool *behavior_pool) {
    assert(m);
    assert(name != NULL);
    assert(port >= 0);
    assert(is_listen_thread());

    // See if we've already got a proxy running with that name and port,
    // and create one if needed.
    //
    bool found = false;

    pthread_mutex_lock(&m->proxy_main_lock);

    proxy *p = m->proxy_head;
    while (p != NULL && !found) {
        pthread_mutex_lock(&p->proxy_lock);

        assert(p->port > 0);
        assert(p->name != NULL);

        found = ((p->port == port) &&
                 (strcmp(p->name, name) == 0));

        pthread_mutex_unlock(&p->proxy_lock);

        if (found) {
            break;
        }

        p = p->next;
    }

    pthread_mutex_unlock(&m->proxy_main_lock);

    if (p == NULL) {
        p = cproxy_create(m, name, port,
                          config,
                          config_ver,
                          behavior_pool,
                          m->nthreads);
        if (p != NULL) {
            pthread_mutex_lock(&m->proxy_main_lock);

            p->next = m->proxy_head;
            m->proxy_head = p;

            pthread_mutex_unlock(&m->proxy_main_lock);

            int n = cproxy_listen(p);
            if (n > 0) {
                if (settings.verbose > 2) {
                    moxi_log_write(
                            "cproxy_listen success %u for %s to %s with %d conns\n",
                            p->port, p->name, p->config, n);
                }
                m->stat_proxy_starts++;
            } else {
                if (settings.verbose > 1) {
                    moxi_log_write("ERROR: cproxy_listen failed on %u to %s\n",
                            p->port, p->config);
                }
                m->stat_proxy_start_fails++;
            }
        } else {
            if (settings.verbose > 2) {
                moxi_log_write("ERROR: cproxy_create failed on %s, %d, %s\n",
                               name, port, config);
            }
        }
    } else {
        if (settings.verbose > 2) {
            moxi_log_write("conp existing config change %u\n",
                    p->port);
        }

        bool changed  = false;
        bool shutdown_flag = false;

        pthread_mutex_lock(&m->proxy_main_lock);

        // Turn off the front_cache while we're reconfiguring.
        //
        mcache_stop(&p->front_cache);
        matcher_stop(&p->front_cache_matcher);
        matcher_stop(&p->front_cache_unmatcher);

        matcher_stop(&p->optimize_set_matcher);

        pthread_mutex_lock(&p->proxy_lock);

        if (settings.verbose > 2) {
            if (p->config && config &&
                strcmp(p->config, config) != 0) {
                moxi_log_write("conp config changed from %s to %s\n",
                        p->config, config);
            }
        }

        changed =
            update_str_config(&p->config, config,
                              "conp config changed") ||
            changed;

        changed =
            (cproxy_equal_behavior(&p->behavior_pool.base,
                                   &behavior_pool->base) == false) ||
            changed;

        p->behavior_pool.base = behavior_pool->base;

        changed =
            update_behaviors_config(&p->behavior_pool.arr,
                                    &p->behavior_pool.num,
                                    behavior_pool->arr,
                                    behavior_pool->num,
                                    "conp behaviors changed") ||
            changed;

        if (p->config != NULL &&
            p->behavior_pool.arr != NULL) {
            m->stat_proxy_existings++;
        } else {
            m->stat_proxy_shutdowns++;
            shutdown_flag = true;
        }

        assert(config_ver != p->config_ver);

        p->config_ver = config_ver;

        pthread_mutex_unlock(&p->proxy_lock);

        if (settings.verbose > 2) {
            moxi_log_write("conp changed %s, shutdown %s\n",
                    changed ? "true" : "false",
                    shutdown_flag ? "true" : "false");
        }

        // Restart the front_cache, if necessary.
        //
        if (shutdown_flag == false) {
            if (behavior_pool->base.front_cache_max > 0 &&
                behavior_pool->base.front_cache_lifespan > 0) {
                mcache_start(&p->front_cache,
                             behavior_pool->base.front_cache_max);

                if (strlen(behavior_pool->base.front_cache_spec) > 0) {
                    matcher_start(&p->front_cache_matcher,
                                  behavior_pool->base.front_cache_spec);
                }

                if (strlen(behavior_pool->base.front_cache_unspec) > 0) {
                    matcher_start(&p->front_cache_unmatcher,
                                  behavior_pool->base.front_cache_unspec);
                }
            }

            if (strlen(behavior_pool->base.optimize_set) > 0) {
                matcher_start(&p->optimize_set_matcher,
                              behavior_pool->base.optimize_set);
            }
        }

        // Send update across worker threads, avoiding locks.
        //
        for (int i = 1; i < m->nthreads; i++) {
            LIBEVENT_THREAD *t = thread_by_index(i);
            assert(t);
            assert(t->work_queue);

            proxy_td *ptd = &p->thread_data[i];
            if (t &&
                t->work_queue) {
                work_send(t->work_queue, update_ptd_config, ptd, NULL);
            }
        }

        pthread_mutex_unlock(&m->proxy_main_lock);

        if (settings.verbose > 2) {
            moxi_log_write("conp changed %s, %d\n",
                           changed ? "true" : "false", config_ver);
        }
    }
}

// ----------------------------------------------------------

static void update_ptd_config(void *data0, void *data1) {
    (void)data1;

    proxy_td *ptd = data0;
    assert(ptd);

    proxy *p = ptd->proxy;
    assert(p);

    assert(is_listen_thread() == false); // Expecting a worker thread.

    pthread_mutex_lock(&p->proxy_lock);

    bool changed = false;
    int  port = p->port;
    int  prev = ptd->config_ver;

    if (ptd->config_ver != p->config_ver) {
        ptd->config_ver = p->config_ver;

        changed =
            update_str_config(&ptd->config, p->config, NULL) ||
            changed;

        ptd->behavior_pool.base = p->behavior_pool.base;

        changed =
            update_behaviors_config(&ptd->behavior_pool.arr,
                                    &ptd->behavior_pool.num,
                                    p->behavior_pool.arr,
                                    p->behavior_pool.num,
                                    NULL) ||
            changed;
    }

    pthread_mutex_unlock(&p->proxy_lock);

    // Restart the key_stats, if necessary.
    //
    if (changed) {
        mcache_stop(&ptd->key_stats);
        matcher_stop(&ptd->key_stats_matcher);
        matcher_stop(&ptd->key_stats_unmatcher);

        if (ptd->config != NULL) {
            if (ptd->behavior_pool.base.key_stats_max > 0 &&
                ptd->behavior_pool.base.key_stats_lifespan > 0) {
                mcache_start(&ptd->key_stats,
                             ptd->behavior_pool.base.key_stats_max);

                if (strlen(ptd->behavior_pool.base.key_stats_spec) > 0) {
                    matcher_start(&ptd->key_stats_matcher,
                                  ptd->behavior_pool.base.key_stats_spec);
                }

                if (strlen(ptd->behavior_pool.base.key_stats_unspec) > 0) {
                    matcher_start(&ptd->key_stats_unmatcher,
                                  ptd->behavior_pool.base.key_stats_unspec);
                }
            }
        }

        if (settings.verbose > 2) {
            moxi_log_write("update_ptd_config %u, %u to %u\n",
                    port, prev, ptd->config_ver);
        }
    } else {
        if (settings.verbose > 2) {
            moxi_log_write("update_ptd_config %u, %u = %u no change\n",
                    port, prev, ptd->config_ver);
        }
    }
}

// ----------------------------------------------------------

static bool update_str_config(char **curr, char *next, char *descrip) {
    bool rv = false;

    if ((*curr != NULL) &&
        (next == NULL ||
         strcmp(*curr, next) != 0)) {
        free(*curr);
        *curr = NULL;

        rv = true;

        if (descrip != NULL &&
            settings.verbose > 2) {
            moxi_log_write("%s\n", descrip);
        }
    }
    if (*curr == NULL && next != NULL) {
        *curr = trimstrdup(next);
    }

    return rv;
}

static bool update_behaviors_config(proxy_behavior **curr,
                                    int  *curr_num,
                                    proxy_behavior  *next,
                                    int   next_num,
                                    char *descrip) {
    bool rv = false;

    if ((*curr != NULL) &&
        (next == NULL ||
         cproxy_equal_behaviors(*curr_num,
                                *curr,
                                next_num,
                                next) == false)) {
        free(*curr);
        *curr = NULL;
        *curr_num = 0;

        rv = true;

        if (descrip != NULL &&
            settings.verbose > 2) {
            moxi_log_write("%s\n", descrip);
        }
    }
    if (*curr == NULL && next != NULL) {
        *curr = cproxy_copy_behaviors(next_num,
                                      next);
        *curr_num = next_num;
    }

    return rv;
}

// ----------------------------------------------------------

/**
 * Parse server-level behaviors from a pool into a given
 * array of behaviors, one entry for each server.
 *
 * An example prefix is "svr".
 */
char *parse_kvs_servers(char *prefix,
                        char *pool_name,
                        kvpair_t *kvs,
                        char **servers,
                        proxy_behavior_pool *behavior_pool) {
    assert(prefix);
    assert(pool_name);
    assert(kvs);
    assert(servers);
    assert(behavior_pool);
    assert(behavior_pool->arr);

    if (behavior_pool->num <= 0) {
        return NULL;
    }

    // Create a config string that libmemcached likes.
    // See memcached_servers_parse().
    //
    int   config_len = 200;
    char *config_str = calloc(config_len, 1);

    for (int j = 0; servers[j]; j++) {
        assert(j < behavior_pool->num);

        // Inherit default behavior.
        //
        behavior_pool->arr[j] = behavior_pool->base;

        parse_kvs_behavior(kvs, prefix, servers[j],
                           &behavior_pool->arr[j]);

        // Grow config string for libmemcached.
        //
        int x = 40 + // For port and weight.
            strlen(config_str) +
            strlen(behavior_pool->arr[j].host);
        if (config_len < x) {
            config_len = 2 * (config_len + x);
            config_str = realloc(config_str, config_len);
        }

        char *config_end = config_str + strlen(config_str);
        if (config_end != config_str) {
            *config_end++ = ',';
        }

        if (strlen(behavior_pool->arr[j].host) > 0 &&
            behavior_pool->arr[j].port > 0) {
            snprintf(config_end,
                     config_len - (config_end - config_str),
                     "%s:%u",
                     behavior_pool->arr[j].host,
                     behavior_pool->arr[j].port);
        } else {
            if (settings.verbose > 1) {
                moxi_log_write("ERROR: missing host:port for svr-%s in %s\n",
                        servers[j], pool_name);
            }
        }

        if (behavior_pool->arr[j].downstream_weight > 0) {
            config_end = config_str + strlen(config_str);
            snprintf(config_end,
                     config_len - (config_end - config_str),
                     ":%u",
                     behavior_pool->arr[j].downstream_weight);
        }

        if (settings.verbose > 2) {
            cproxy_dump_behavior(&behavior_pool->arr[j],
                                 "pks", 0);
        }
    }

    return config_str;
}

// ----------------------------------------------------------

/**
 * Parse a "[prefix]-[name]" configuration section into a behavior.
 */
char **parse_kvs_behavior(kvpair_t *kvs,
                          char *prefix,
                          char *name,
                          proxy_behavior *behavior) {
    assert(kvs);
    assert(prefix);
    assert(name);
    assert(behavior);

    char key[800];

    snprintf(key, sizeof(key), "%s-%s", prefix, name);

    char **props = get_key_values(kvs, key);
    for (int k = 0; props && props[k]; k++) {
        char *key_val = trimstrdup(props[k]);
        if (key_val != NULL) {
            cproxy_parse_behavior_key_val_str(key_val, behavior);
            free(key_val);
        }
    }

    return props;
}

// ----------------------------------------------------------

char **get_key_values(kvpair_t *kvs, char *key) {
    kvpair_t *x = find_kvpair(kvs, key);
    if (x != NULL) {
        return x->values;
    }
    return NULL;
}

