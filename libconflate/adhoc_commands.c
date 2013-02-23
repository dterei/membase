#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "conflate.h"
#include "conflate_internal.h"
#include "conflate_convenience.h"

bool commands_initialized = false;

static enum conflate_mgmt_cb_result process_serverlist(void *opaque,
                                                       conflate_handle_t *handle,
                                                       const char *cmd,
                                                       bool direct,
                                                       kvpair_t *conf,
                                                       conflate_form_result *r)
{
   (void)opaque;
   (void)cmd;
   (void)r;

    /* If we have "config_is_private" set to "yes" we should only
       process this if it's direct (i.e. ignore pubsub) */
    if (!direct) {
        char *priv = conflate_get_private(handle, "config_is_private",
                                          handle->conf->save_path);

        if (priv && strcmp(priv, "yes") == 0) {
            CONFLATE_LOG(handle, LOG_LVL_INFO,
                         "Currently using a private config, ignoring update.");
            return RV_OK;
        }
        free(priv);
    }

    CONFLATE_LOG(handle, LOG_LVL_INFO, "Processing a serverlist");

    /* Persist the config lists */
    if (!save_kvpairs(handle, conf, handle->conf->save_path)) {
        CONFLATE_LOG(handle, LOG_LVL_ERROR, "Can not save config to %s",
                     handle->conf->save_path);
    }

    /* Send the config to the callback */
    handle->conf->new_config(handle->conf->userdata, conf);

    return RV_OK;
}

static enum conflate_mgmt_cb_result process_set_private(void *opaque,
                                                        conflate_handle_t *handle,
                                                        const char *cmd,
                                                        bool direct,
                                                        kvpair_t *form,
                                                        conflate_form_result *r)
{
   (void)opaque;
   (void)cmd;
   (void)r;

    /* Only direct stat requests are handled. */
    assert(direct);
    enum conflate_mgmt_cb_result rv = RV_ERROR;

    char *key = get_simple_kvpair_val(form, "key");
    char *value = get_simple_kvpair_val(form, "value");

    if (key && value) {
        if (conflate_save_private(handle, key, value,
                                  handle->conf->save_path)) {
            rv = RV_OK;
        }
    } else {
        rv = RV_BADARG;
    }

    return rv;
}

static enum conflate_mgmt_cb_result process_get_private(void *opaque,
                                                        conflate_handle_t *handle,
                                                        const char *cmd,
                                                        bool direct,
                                                        kvpair_t *form,
                                                        conflate_form_result *r)
{
   (void)opaque;
   (void)cmd;

    /* Only direct stat requests are handled. */
    assert(direct);
    enum conflate_mgmt_cb_result rv = RV_ERROR;

    char *key = get_simple_kvpair_val(form, "key");

    if (key) {
        /* Initialize the form so there's always one there */
        conflate_init_form(r);
        char *value = conflate_get_private(handle, key,
                                           handle->conf->save_path);
        if (value) {
            conflate_add_field(r, key, value);
            free(value);
        }

        rv = RV_OK;
    } else {
        rv = RV_BADARG;
    }

    return rv;
}

static enum conflate_mgmt_cb_result process_delete_private(void *opaque,
                                                           conflate_handle_t *handle,
                                                           const char *cmd,
                                                           bool direct,
                                                           kvpair_t *form,
                                                           conflate_form_result *r)
{
   (void)opaque;
   (void)cmd;
   (void)r;

    /* Only direct stat requests are handled. */
    assert(direct);
    enum conflate_mgmt_cb_result rv = RV_ERROR;

    char *key = get_simple_kvpair_val(form, "key");

    if (key) {
        if (conflate_delete_private(handle, key,
                                    handle->conf->save_path)) {
            rv = RV_OK;
        }
    } else {
        rv = RV_BADARG;
    }

    return rv;
}

void conflate_init_commands(void)
{
    if (commands_initialized) {
        return;
    }

    conflate_register_mgmt_cb("set_private",
                              "Set a private value on the agent.",
                              process_set_private);
    conflate_register_mgmt_cb("get_private",
                              "Get a private value from the agent.",
                              process_get_private);
    conflate_register_mgmt_cb("rm_private",
                              "Delete a private value from the agent.",
                              process_delete_private);

    conflate_register_mgmt_cb("serverlist", "Configure a server list.",
                              process_serverlist);

    commands_initialized = true;
}
