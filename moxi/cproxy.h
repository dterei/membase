/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#ifndef CPROXY_H
#define CPROXY_H

#include "genhash.h"
#include "work.h"
#include "matcher.h"
#include "mcs.h"
#include "htgram.h"

// From libmemcached.
//
uint32_t murmur_hash(const char *key, size_t length);

// -------------------------------

int cproxy_init(char *cfg_str,
                char *behavior_str,
                int nthreads,
                struct event_base *main_base);

#define CPROXY_NOT_CAS UINT64_MAX

extern volatile uint64_t msec_current_time;

uint64_t usec_now(void);

extern char cproxy_hostname[300]; // Immutable after init.

// -------------------------------

// Special bucket name for the null bucket.
//
#define NULL_BUCKET "[ <NULL_BUCKET> ]"

// Special bucket name that signifies that
// upstream connections start on the first
// configured bucket.
//
#define FIRST_BUCKET "[ <FIRST_BUCKET> ]"

// -------------------------------

typedef struct {
    char *(*item_key)(void *it);
    int   (*item_key_len)(void *it);
    int   (*item_len)(void *it);
    void  (*item_add_ref)(void *it);
    void  (*item_dec_ref)(void *it);
    void *(*item_get_next)(void *it);
    void  (*item_set_next)(void *it, void *next);
    void *(*item_get_prev)(void *it);
    void  (*item_set_prev)(void *it, void *prev);
    uint64_t (*item_get_exptime)(void *it);
    void     (*item_set_exptime)(void *it, uint64_t exptime);
} mcache_funcs;

extern mcache_funcs mcache_item_funcs;
extern mcache_funcs mcache_key_stats_funcs;

typedef struct {
    mcache_funcs *funcs;

    pthread_mutex_t *lock; // NULL-able, for non-multithreaded.

    bool key_alloc;        // True if mcache must alloc key memory.

    genhash_t *map;        // NULL-able, keyed by string, value is item.

    uint32_t max;          // Maxiumum number of items to keep.

    void *lru_head;        // Most recently used.
    void *lru_tail;        // Least recently used.

    uint32_t oldest_live;  // In millisecs, relative to msec_current_time.

    // Statistics.
    //
    uint64_t tot_get_hits;
    uint64_t tot_get_expires;
    uint64_t tot_get_misses;
    uint64_t tot_get_bytes;
    uint64_t tot_adds;
    uint64_t tot_add_skips;
    uint64_t tot_add_fails;
    uint64_t tot_add_bytes;
    uint64_t tot_deletes;
    uint64_t tot_evictions;
} mcache;

typedef struct proxy               proxy;
typedef struct proxy_td            proxy_td;
typedef struct proxy_main          proxy_main;
typedef struct proxy_stats         proxy_stats;
typedef struct proxy_behavior      proxy_behavior;
typedef struct proxy_behavior_pool proxy_behavior_pool;
typedef struct downstream          downstream;
typedef struct key_stats           key_stats;

struct proxy_behavior {
    // IL means startup, system initialization level behavior.
    // ML means proxy/pool manager-level behavior (proxy_main).
    // PL means proxy/pool-level behavior.
    // SL means server-level behavior, although we inherit from proxy level.
    //
    uint32_t       cycle;               // IL: Clock resolution in millisecs.
    uint32_t       downstream_max;      // PL: Downstream concurrency.
    uint32_t       downstream_conn_max; // PL: Max # of conns per thread
                                        // and per host_ident.
    uint32_t       downstream_weight;   // SL: Server weight.
    uint32_t       downstream_retry;    // SL: How many times to retry a cmd.
    enum protocol  downstream_protocol; // SL: Favored downstream protocol.
    struct timeval downstream_timeout;  // SL: Fields of 0 mean no timeout.
    struct timeval downstream_conn_queue_timeout; // SL: Fields of 0 mean no timeout.
    struct timeval wait_queue_timeout;  // PL: Fields of 0 mean no timeout.
    struct timeval connect_timeout;     // PL: Fields of 0 mean no timeout.
    struct timeval auth_timeout;        // PL: Fields of 0 mean no timeout.
    bool           time_stats;          // IL: Capture timing stats.
    char           mcs_opts[80];        // PL: Extra options for mcs initialization.

    uint32_t connect_max_errors;      // IL: Pause when too many connect() errs.
    uint32_t connect_retry_interval;  // IL: Time in millisecs before retrying
                                      // when too many connect() errors, to not
                                      // overwhelm the downstream servers.

    uint32_t front_cache_max;         // PL: Max # of front cachable items.
    uint32_t front_cache_lifespan;    // PL: In millisecs.
    char     front_cache_spec[300];   // PL: Matcher prefixes for front caching.
    char     front_cache_unspec[100]; // PL: Don't front cache prefixes.

    uint32_t key_stats_max;         // PL: Max # of key stats entries.
    uint32_t key_stats_lifespan;    // PL: In millisecs.
    char     key_stats_spec[300];   // PL: Matcher prefixes for key-level stats.
    char     key_stats_unspec[100]; // PL: Don't key stat prefixes.

    char optimize_set[400]; // PL: Matcher prefixes for SET optimization.

    char usr[250];    // SL.
    char pwd[900];    // SL.
    char host[250];   // SL.
    int  port;        // SL.
    char bucket[250]; // SL.
    char nodeLocator[20]; // Ex: ketama or vbucket.

    // ML: Port for proxy_main to listen on.
    //
    int port_listen;

    char default_bucket_name[250]; // ML: The named bucket (proxy->name)
                                   // that upstream conn's should start on.
                                   // When empty (""), then only binary SASL
                                   // clients can actually do anything useful.
};

proxy_behavior behavior_default_g;

struct proxy_behavior_pool {
    proxy_behavior  base; // Proxy pool-level (PL) behavior.
    int             num;  // Number of server-level (SL) behaviors.
    proxy_behavior *arr;  // Array, size is num.
};

typedef enum {
    PROXY_CONF_TYPE_STATIC = 0,
    PROXY_CONF_TYPE_DYNAMIC,
    PROXY_CONF_TYPE_last
} enum_proxy_conf_type;

// Quick map of struct hierarchy...
//
// proxy_main
//  - has list of...
//    proxy
//     - has array of...
//       proxy_td (one proxy_td per worker thread)
//       - has list of...
//         downstream (in either reserved or released list)
//         - has mst/libmemcached struct
//         - has array of downstream conn's
//         - has non-NULL upstream conn, when reserved

/* Structure used and owned by main listener thread to
 * track all the outstanding proxy objects.
 */
struct proxy_main {
    proxy_behavior behavior; // Default, main listener modifiable only.

    enum_proxy_conf_type conf_type; // Immutable proxy configuration type.

    // Any thread that accesses the proxy list must
    // first acquire the proxy_main_lock.
    //
    pthread_mutex_t proxy_main_lock;

    // Start of proxy list.  Covered by proxy_main_lock.
    // Only the main listener thread may modify the proxy list.
    // Other threads may read-only traverse the proxy list.
    //
    proxy *proxy_head;

    int nthreads; // Immutable.

    // Updated by main listener thread only,
    // so no extra locking needed.
    //
    uint64_t stat_configs;
    uint64_t stat_config_fails;
    uint64_t stat_proxy_starts;
    uint64_t stat_proxy_start_fails;
    uint64_t stat_proxy_existings;
    uint64_t stat_proxy_shutdowns;
};

/* Owned by main listener thread.
 */
struct proxy {
    proxy_main *main; // Immutable, points to parent proxy_main.

    int   port;   // Immutable.
    char *name;   // Mutable, covered by proxy_lock, for debugging, NULL-able.
    char *config; // Mutable, covered by proxy_lock, mem owned by proxy,
                  // might be NULL if the proxy is shutting down.

    // Mutable, covered by proxy_lock, incremented
    // whenever config changes.
    //
    uint32_t config_ver;

    // Mutable, covered by proxy_lock.
    //
    proxy_behavior_pool behavior_pool;

    // Any thread that accesses the mutable fields should
    // first acquire the proxy_lock.
    //
    pthread_mutex_t proxy_lock;

    // Number of listening conn's acting as a proxy,
    // where (((proxy *) conn->extra) == this).
    // Modified/accessed only by main listener thread.
    //
    uint64_t listening;
    uint64_t listening_failed; // When server_socket() failed.

    proxy *next; // Modified/accessed only by main listener thread.

    mcache  front_cache;
    matcher front_cache_matcher;
    matcher front_cache_unmatcher;

    matcher optimize_set_matcher;

    proxy_td *thread_data;     // Immutable.
    int       thread_data_num; // Immutable.
};

struct proxy_stats {
    // Naming convention is that num_xxx's go up and down,
    // while tot_xxx's and err_xxx's only increase.  Only
    // the tot_xxx's and err_xxx's can be reset to 0.
    //
    uint64_t num_upstream; // Current # of upstreams conns using this proxy.
    uint64_t tot_upstream; // Total # upstream conns that used this proxy.

    uint64_t num_downstream_conn;
    uint64_t tot_downstream_conn;
    uint64_t tot_downstream_conn_acquired;
    uint64_t tot_downstream_conn_released;
    uint64_t tot_downstream_released;
    uint64_t tot_downstream_reserved;
    uint64_t tot_downstream_reserved_time;
    uint64_t max_downstream_reserved_time;
    uint64_t tot_downstream_freed;
    uint64_t tot_downstream_quit_server;
    uint64_t tot_downstream_max_reached;
    uint64_t tot_downstream_create_failed;

    // When connections have stabilized...
    //   tot_downstream_connect_started ==
    //     tot_downstream_connect + tot_downstream_connect_failed.
    //
    // When a new connection is just created but not yet ready for use...
    //   tot_downstream_connect_started >
    //     tot_downstream_connect + tot_downstream_connect_failed.
    //
    uint64_t tot_downstream_connect_started;
    uint64_t tot_downstream_connect_wait;
    uint64_t tot_downstream_connect; // Incremented when connect() + auth +
                                     // bucket_selection succeeds.
    uint64_t tot_downstream_connect_failed;
    uint64_t tot_downstream_connect_timeout;
    uint64_t tot_downstream_connect_interval;
    uint64_t tot_downstream_connect_max_reached;

    uint64_t tot_downstream_waiting_errors;
    uint64_t tot_downstream_auth;
    uint64_t tot_downstream_auth_failed;
    uint64_t tot_downstream_bucket;
    uint64_t tot_downstream_bucket_failed;
    uint64_t tot_downstream_propagate_failed;
    uint64_t tot_downstream_close_on_upstream_close;
    uint64_t tot_downstream_conn_queue_timeout;
    uint64_t tot_downstream_conn_queue_add;
    uint64_t tot_downstream_conn_queue_remove;
    uint64_t tot_downstream_timeout;
    uint64_t tot_wait_queue_timeout;
    uint64_t tot_auth_timeout;
    uint64_t tot_assign_downstream;
    uint64_t tot_assign_upstream;
    uint64_t tot_assign_recursion;
    uint64_t tot_reset_upstream_avail;
    uint64_t tot_retry;
    uint64_t tot_retry_time;
    uint64_t max_retry_time;
    uint64_t tot_retry_vbucket;
    uint64_t tot_upstream_paused;
    uint64_t tot_upstream_unpaused;
    uint64_t tot_multiget_keys;
    uint64_t tot_multiget_keys_dedupe;
    uint64_t tot_multiget_bytes_dedupe;
    uint64_t tot_optimize_sets;
    uint64_t err_oom;
    uint64_t err_upstream_write_prep;
    uint64_t err_downstream_write_prep;
    uint64_t tot_cmd_time;
    uint64_t tot_cmd_count;
    uint64_t tot_local_cmd_time;
    uint64_t tot_local_cmd_count;
};

typedef struct {
    uint64_t seen;        // Number of times a command was seen.
    uint64_t hits;        // Number of hits or successes.
    uint64_t misses;      // Number of misses or failures.
    uint64_t read_bytes;  // Total bytes read, incoming into proxy.
    uint64_t write_bytes; // Total bytes written, outgoing from proxy.
    uint64_t cas;         // Number that had or required cas-id.
} proxy_stats_cmd;

typedef enum {
    STATS_CMD_GET = 0, // For each "get" cmd, even if multikey get.
    STATS_CMD_GET_KEY, // For each key in a "get".
    STATS_CMD_SET,
    STATS_CMD_ADD,
    STATS_CMD_REPLACE,
    STATS_CMD_DELETE,
    STATS_CMD_APPEND,
    STATS_CMD_PREPEND,
    STATS_CMD_INCR,
    STATS_CMD_DECR,
    STATS_CMD_FLUSH_ALL,
    STATS_CMD_CAS,
    STATS_CMD_STATS,
    STATS_CMD_STATS_RESET,
    STATS_CMD_VERSION,
    STATS_CMD_VERBOSITY,
    STATS_CMD_QUIT,
    STATS_CMD_GETL,
    STATS_CMD_UNL,
    STATS_CMD_ERROR,
    STATS_CMD_last
} enum_stats_cmd;

typedef enum {
    STATS_CMD_TYPE_REGULAR = 0,
    STATS_CMD_TYPE_QUIET,
    STATS_CMD_TYPE_last
} enum_stats_cmd_type;

typedef struct {
    proxy_stats     stats;
    proxy_stats_cmd stats_cmd[STATS_CMD_TYPE_last][STATS_CMD_last];

    HTGRAM_HANDLE downstream_reserved_time_htgram;
    HTGRAM_HANDLE downstream_connect_time_htgram;
} proxy_stats_td;

struct key_stats {
    char key[KEY_MAX_LENGTH + 1];
    int  refcount;
    uint64_t exptime;
    uint64_t added_at;
    key_stats *next;
    key_stats *prev;
    proxy_stats_cmd stats_cmd[STATS_CMD_TYPE_last][STATS_CMD_last];
};

/* We mirror memcached's threading model with a separate
 * proxy_td (td means "thread data") struct owned by each
 * worker thread.  The idea is to avoid extraneous locks.
 */
struct proxy_td { // Per proxy, per worker-thread data struct.
    proxy *proxy; // Immutable parent pointer.

    // Snapshot of proxy-level configuration to avoid locks.
    //
    char    *config;
    uint32_t config_ver;

    proxy_behavior_pool behavior_pool;

    // Upstream conns that are paused, waiting for
    // an available, released downstream.
    //
    conn *waiting_any_downstream_head;
    conn *waiting_any_downstream_tail;

    downstream *downstream_reserved; // Downstreams assigned to upstream conns.
    downstream *downstream_released; // Downstreams unassigned to upstreams conn.
    uint64_t    downstream_tot;      // Total lifetime downstreams created.
    int         downstream_num;      // Number downstreams existing.
    int         downstream_max;      // Max downstream concurrency number.
    uint64_t    downstream_assigns;  // Track recursion.

    // A timeout for the wait_queue, so that we can emit error
    // on any upstream conn's that are waiting too long for
    // an available downstream.
    //
    // Timeout is in use when timeout_tv fields are non-zero.
    //
    struct timeval timeout_tv;
    struct event   timeout_event;

    mcache  key_stats;
    matcher key_stats_matcher;
    matcher key_stats_unmatcher;

    proxy_stats_td stats;
};

/* A 'downstream' struct represents a set of downstream connections.
 * A possibly better name for it should have been "downstream_conn_set".
 *
 * Owned by worker thread.
 */
struct downstream {
    // The following group of fields are immutable or read-only (RO),
    // except for config_ver, which gets updated if the downstream's
    // config/behaviors still matches the parent ptd's config/behaviors.
    //
    proxy_td       *ptd;           // RO: Parent pointer.
    char           *config;        // RO: Mem owned by downstream.
    uint32_t        config_ver;    // RW: Mutable, copy of proxy->config_ver.
    int             behaviors_num; // RO: Snapshot of ptd->behavior_pool.num.
    proxy_behavior *behaviors_arr; // RO: Snapshot of ptd->behavior_pool.arr.
    mcs_st          mst;           // RW: From mcs.

    downstream *next; // To track reserved/released lists.
                      // See ptd->downstream_reserved/downstream_released.

    downstream *next_waiting; // To track lists when a downstream is reserved,
                              // but is waiting for a downstream connection,
                              // per zstored perf enhancement.

    conn **downstream_conns;  // Wraps the fd's of mst with conns.
    int    downstream_used;   // Number of in-use downstream conns, might
                              // be >1 during scatter-gather commands.
    int    downstream_used_start;

    uint64_t usec_start; // Snapshot of usec_now().

    conn  *upstream_conn;     // Non-NULL when downstream is reserved.
    char  *upstream_suffix;   // Last bit to write when downstreams are done.
    int    upstream_suffix_len; // When > 0, overrides strlen(upstream_suffix),
                                // during binary protocol.

    // Used during an error when upstream is binary protocol.
    protocol_binary_response_status upstream_status;

    int    upstream_retry;    // Will be >0 if we should retry the entire
                              // command again when all downstreams are done.
                              // Used in not-my-vbucket error case.  During
                              // the retry, we'll reuse the same multiget
                              // de-duplication tracking table to avoid
                              // asking for successful keys again.
    int    upstream_retries;  // Count number of upstream_retry attempts.

    // Used when proxying a simple, single-key (non-broadcast) command.
    char *target_host_ident;

    genhash_t *multiget; // Keyed by string.
    genhash_t *merger;   // Keyed by string, for merging replies like STATS.

    // Timeout is in use when timeout_tv fields are non-zero.
    //
    struct timeval timeout_tv;
    struct event   timeout_event;
};

// Sentinel value for downstream->downstream_conns[] array entries,
// which usually signals that moxi wasn't able to create a connection
// to a downstream server.
//
#define NULL_CONN ((conn *) -1)

// Functions.
//
proxy *cproxy_create(proxy_main *main,
                     char    *name,
                     int      port,
                     char    *config,
                     uint32_t config_ver,
                     proxy_behavior_pool *behavior_pool,
                     int nthreads);

int cproxy_listen(proxy *p);
int cproxy_listen_port(int port,
                       enum protocol protocol,
                       enum network_transport transport,
                       void       *conn_extra,
                       conn_funcs *conn_funcs);

proxy_td *cproxy_find_thread_data(proxy *p, pthread_t thread_id);
bool      cproxy_init_upstream_conn(conn *c);
bool      cproxy_init_downstream_conn(conn *c);
void      cproxy_on_close_upstream_conn(conn *c);
void      cproxy_on_close_downstream_conn(conn *c);
void      cproxy_on_pause_downstream_conn(conn *c);

void cproxy_upstream_state_change(conn *c, enum conn_states next_state);

void cproxy_add_downstream(proxy_td *ptd);
void cproxy_free_downstream(downstream *d);

downstream *cproxy_create_downstream(char *config,
                                     uint32_t config_ver,
                                     proxy_behavior_pool *behavior_pool);

downstream *cproxy_reserve_downstream(proxy_td *ptd);
bool        cproxy_release_downstream(downstream *d, bool force);
void        cproxy_release_downstream_conn(downstream *d, conn *c);
bool        cproxy_check_downstream_config(downstream *d);

int   cproxy_connect_downstream(downstream *d,
                                LIBEVENT_THREAD *thread,
                                int server_index);
conn *cproxy_connect_downstream_conn(downstream *d,
                                     LIBEVENT_THREAD *thread,
                                     mcs_server_st *msst,
                                     proxy_behavior *behavior);

void  cproxy_wait_any_downstream(proxy_td *ptd, conn *c);
void  cproxy_assign_downstream(proxy_td *ptd);

proxy *cproxy_find_proxy_by_auth(proxy_main *m,
                                 const char *usr,
                                 const char *pwd);

int cproxy_auth_downstream(mcs_server_st *server,
                           proxy_behavior *behavior, int fd);
int cproxy_bucket_downstream(mcs_server_st *server,
                             proxy_behavior *behavior, int fd);

void  cproxy_pause_upstream_for_downstream(proxy_td *ptd, conn *upstream);
conn *cproxy_find_downstream_conn(downstream *d, char *key, int key_length,
                                  bool *local);
conn *cproxy_find_downstream_conn_ex(downstream *d, char *key, int key_length,
                                     bool *local, int *vbucket);
int   cproxy_server_index(downstream *d, char *key, size_t key_length, int *vbucket);
bool  cproxy_prep_conn_for_write(conn *c);
bool  cproxy_dettach_if_noreply(downstream *d, conn *uc);

void cproxy_reset_upstream(conn *uc);

bool cproxy_update_event_write(downstream *d, conn *c);

bool cproxy_forward(downstream *d);

void upstream_error_msg(conn *uc, char *ascii_msg,
                        protocol_binary_response_status binary_status);
void upstream_retry(void *data0, void *data1);

int downstream_conn_index(downstream *d, conn *c);

void cproxy_dump_header(int prefix, char *bb);

int cproxy_max_retries(downstream *d);

// ---------------------------------------------------------------

void cproxy_process_upstream_ascii(conn *c, char *line);
void cproxy_process_upstream_ascii_nread(conn *c);

void cproxy_process_downstream_ascii(conn *c, char *line);
void cproxy_process_downstream_ascii_nread(conn *c);

void cproxy_process_upstream_binary(conn *c);
void cproxy_process_upstream_binary_nread(conn *c);

void cproxy_process_downstream_binary(conn *c);
void cproxy_process_downstream_binary_nread(conn *c);

// ---------------------------------------------------------------
// a2a means ascii upstream, ascii downstream.
//
void cproxy_init_a2a(void);
void cproxy_process_a2a_downstream(conn *c, char *line);
void cproxy_process_a2a_downstream_nread(conn *c);

bool cproxy_forward_a2a_downstream(downstream *d);

bool cproxy_forward_a2a_multiget_downstream(downstream *d, conn *uc);
bool cproxy_forward_a2a_simple_downstream(downstream *d, char *command,
                                          conn *uc);
bool cproxy_forward_a2a_item_downstream(downstream *d, short cmd,
                                        item *it, conn *uc);
bool cproxy_broadcast_a2a_downstream(downstream *d, char *command,
                                     conn *uc, char *suffix);

// ---------------------------------------------------------------
// a2b means ascii upstream, binary downstream.
//
void cproxy_init_a2b(void);
void cproxy_process_a2b_downstream(conn *c);
void cproxy_process_a2b_downstream_nread(conn *c);

bool cproxy_forward_a2b_downstream(downstream *d);
bool cproxy_forward_a2b_multiget_downstream(downstream *d, conn *uc);
bool cproxy_forward_a2b_simple_downstream(downstream *d, char *command,
                                          conn *uc);
bool cproxy_forward_a2b_item_downstream(downstream *d, short cmd,
                                        item *it, conn *uc);
bool cproxy_broadcast_a2b_downstream(downstream *d,
                                     protocol_binary_request_header *req,
                                     int req_size,
                                     uint8_t *key,
                                     uint16_t keylen,
                                     uint8_t  extlen,
                                     conn *uc, char *suffix);

// ---------------------------------------------------------------
// b2b means binary upstream, binary downstream.
//
void cproxy_init_b2b(void);
void cproxy_process_b2b_downstream(conn *c);
void cproxy_process_b2b_downstream_nread(conn *c);

bool cproxy_forward_b2b_downstream(downstream *d);
bool cproxy_forward_b2b_multiget_downstream(downstream *d, conn *uc);
bool cproxy_forward_b2b_simple_downstream(downstream *d, conn *uc);

bool cproxy_broadcast_b2b_downstream(downstream *d, conn *uc);

// ---------------------------------------------------------------

bool b2b_forward_item(conn *uc, downstream *d, item *it);

bool b2b_forward_item_vbucket(conn *uc, downstream *d, item *it,
                              conn *c, int vbucket);

// ---------------------------------------------------------------

// Magic opaque value that tells us to eat a binary quiet command
// response.  That is, do not send the response up to the ascii client
// which originally made its request with noreply.
//
#define OPAQUE_IGNORE_REPLY 0x0411F00D

bool cproxy_binary_ignore_reply(conn *c, protocol_binary_response_header *header, item *it);

// ---------------------------------------------------------------

proxy_main *cproxy_gen_proxy_main(proxy_behavior behavior,
                                  int nthreads, enum_proxy_conf_type conf_type);

proxy_behavior cproxy_parse_behavior(char          *behavior_str,
                                     proxy_behavior behavior_default);

void cproxy_parse_behavior_key_val_str(char *key_val,
                                       proxy_behavior *behavior);

void cproxy_parse_behavior_key_val(char *key,
                                   char *val,
                                   proxy_behavior *behavior);

proxy_behavior *cproxy_copy_behaviors(int arr_size, proxy_behavior *arr);

bool cproxy_equal_behaviors(int x_size, proxy_behavior *x,
                            int y_size, proxy_behavior *y);
bool cproxy_equal_behavior(proxy_behavior *x,
                           proxy_behavior *y);

void cproxy_dump_behavior(proxy_behavior *b, char *prefix, int level);
void cproxy_dump_behavior_ex(proxy_behavior *b, char *prefix, int level,
                             void (*dump)(const void *dump_opaque,
                                          const char *prefix,
                                          const char *key,
                                          const char *buf),
                             const void *dump_opaque);
void cproxy_dump_behavior_stderr(const void *dump_opaque,
                                 const char *prefix,
                                 const char *key,
                                 const char *val);

// ---------------------------------------------------------------

bool cproxy_is_broadcast_cmd(int cmd);

void cproxy_ascii_broadcast_suffix(downstream *d);

void cproxy_upstream_ascii_item_response(item *it, conn *uc,
                                         int cas_emit);

bool cproxy_clear_timeout(downstream *d);

struct timeval cproxy_get_downstream_timeout(downstream *d, conn *c);

bool cproxy_start_downstream_timeout(downstream *d, conn *c);
bool cproxy_start_downstream_timeout_ex(downstream *d, conn *c,
                                        struct timeval dt);
bool cproxy_start_wait_queue_timeout(proxy_td *ptd, conn *uc);

rel_time_t cproxy_realtime(const time_t exptime);

void cproxy_close_conn(conn *c);

void cproxy_reset_stats_td(proxy_stats_td *pstd);
void cproxy_reset_stats(proxy_stats *ps);
void cproxy_reset_stats_cmd(proxy_stats_cmd *sc);

bool cproxy_binary_cork_cmd(conn *uc);
void cproxy_binary_uncork_cmds(downstream *d, conn *uc);

bool ascii_scan_key(char *line, char **key, int *key_len);

// Multiget key de-duplication.
//
typedef struct multiget_entry multiget_entry;

struct multiget_entry {
    conn           *upstream_conn;
    uint32_t        opaque; // For binary protocol.
    uint64_t        hits;
    multiget_entry *next;
};

bool multiget_ascii_downstream(
    downstream *d, conn *uc,
    int (*emit_start)(conn *c, char *cmd, int cmd_len),
    int (*emit_skey)(conn *c, char *skey, int skey_len, int vbucket, int key_index),
    int (*emit_end)(conn *c),
    mcache *front_cache);

void multiget_ascii_downstream_response(downstream *d, item *it);

void multiget_foreach_free(const void *key,
                           const void *value,
                           void *user_data);

void multiget_remove_upstream(const void *key,
                              const void *value,
                              void *user_data);

// Space or null terminated key funcs.
//
size_t skey_len(const char *key);
int    skey_hash(const void *v);
int    skey_equal(const void *v1, const void *v2);

extern struct hash_ops strhash_ops;
extern struct hash_ops skeyhash_ops;

void noop_free(void *v);

// Stats handling.
//
bool protocol_stats_merge_line(genhash_t *merger, char *line);

bool protocol_stats_merge_name_val(genhash_t *merger,
                                   char *prefix,
                                   int   prefix_len,
                                   char *name,
                                   int   name_len,
                                   char *val,
                                   int   val_len);

void protocol_stats_foreach_free(const void *key,
                                 const void *value,
                                 void *user_data);

void protocol_stats_foreach_write(const void *key,
                                  const void *value,
                                  void *user_data);

bool cproxy_optimize_set_ascii(downstream *d, conn *uc,
                               char *key, int key_len);

void cproxy_del_front_cache_key_ascii(downstream *d,
                                      char *command);

void cproxy_del_front_cache_key_ascii_response(downstream *d,
                                               char *response,
                                               char *command);

void cproxy_front_cache_delete(proxy_td *ptd, char *key, int key_len);

bool cproxy_front_cache_key(proxy_td *ptd, char *key, int key_len);

HTGRAM_HANDLE cproxy_create_timing_histogram(void);

typedef void (*mcache_traversal_func)(const void *it, void *userdata);

// Functions for the front cache.
//
void  mcache_init(mcache *m, bool multithreaded,
                  mcache_funcs *funcs, bool key_alloc);
void  mcache_start(mcache *m, uint32_t max);
bool  mcache_started(mcache *m);
void  mcache_stop(mcache *m);
void  mcache_reset_stats(mcache *m);
void *mcache_get(mcache *m, char *key, int key_len,
                 uint64_t curr_time);
void  mcache_set(mcache *m, void *it,
                 uint64_t exptime,
                 bool add_only,
                 bool mod_exptime_if_exists);
void  mcache_delete(mcache *m, char *key, int key_len);
void  mcache_flush_all(mcache *m, uint32_t msec_exp);
void  mcache_foreach(mcache *m, mcache_traversal_func f, void *userdata);

// Functions for key stats.
//
key_stats *find_key_stats(proxy_td *ptd, char *key, int key_len,
                          uint64_t msec_time);

void touch_key_stats(proxy_td *ptd, char *key, int key_len,
                     uint64_t msec_current_time,
                     enum_stats_cmd_type cmd_type,
                     enum_stats_cmd cmd,
                     int delta_seen,
                     int delta_hits,
                     int delta_misses,
                     int delta_read_bytes,
                     int delta_write_bytes);

void key_stats_add_ref(void *it);
void key_stats_dec_ref(void *it);

// TODO: The following generic items should be broken out into util file.
//
bool  add_conn_item(conn *c, item *it);
char *add_conn_suffix(conn *c);

void *cproxy_make_bin_header(conn *c, uint8_t magic);

protocol_binary_response_header *cproxy_make_bin_error(conn *c,
                                                       uint16_t status);

size_t scan_tokens(char *command, token_t *tokens, const size_t max_tokens,
                   int *command_len);

char *nread_text(short x);

char *skipspace(char *s);
char *trailspace(char *s);
char *trimstr(char *s);
char *trimstrdup(char *s);
bool  wordeq(char *s, char *word);

#endif // CPROXY_H
