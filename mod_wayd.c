/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_main.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_script.h"
#include "http_connection.h"
#include "ap_mpm.h"

#include "apr_md5.h"
#include "apr_strings.h"
#include "apr_optional.h"
#include "apr_lib.h"

#include "mpm_common.h"

/*--------------------------------------------------------------------------*/
/*                                                                          */
/* Data declarations.                                                       */
/*                                                                          */
/* Here are the static cells and structure declarations private to our      */
/* module.                                                                  */
/*                                                                          */
/*--------------------------------------------------------------------------*/

typedef struct wayd_server_config {
    char               *procname;
    size_t              nprocname;
                    
    int                 starttime;
    int                 host;
    int                 uri;
    int                 urilimit;
    int                 args;
    int                 argslimit;
    apr_array_header_t *headers;  /* char* */
} wayd_server_config_t;

/*
 * Declare ourselves so the configuration routines can find and know us.
 * We'll fill it in at the end of the module.
 */
module AP_MODULE_DECLARE_DATA wayd_module;

/*
 * This function gets called to create a per-server configuration
 * record.  It will always be called for the "default" server.
 *
 * The return value is a pointer to the created module-specific
 * structure.
 */
static void *wayd_create_server_config(apr_pool_t *p, server_rec *s)
{
    wayd_server_config_t *config = apr_palloc(p, sizeof(wayd_server_config_t));
    if (!config) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s, "apr_palloc error");
        return NULL;
    }

    config->starttime = 0;
    config->host      = 0;
    config->uri       = 1;
    config->urilimit  = 64;
    config->args      = 0;
    config->argslimit = 64;
    config->headers   = NULL;

    return config;
}

extern char **environ;
static void wayd_child_init(apr_pool_t *p, server_rec *s)
{
    wayd_server_config_t *config;
    config = ap_get_module_config(s->module_config, &wayd_module);

    int    i;
    size_t size = 0;
    for (i = 0; environ[i]; ++i)
        size += strlen(environ[i]) + 1;

    char *raw = malloc(size);
    for (i = 0; environ[i]; ++i) {
        memcpy(raw, environ[i], strlen(environ[i]) + 1);
        environ[i] = raw;
        raw += strlen(environ[i]) + 1;
    }

    char **argv = (char **) s->process->argv;

    for (i = 0; i < s->process->argc; ++i)
        size += strlen(argv[i]) + 1;

    config->procname  = argv[0];
    config->nprocname = size-1;
    argv[1] = 0;

    /* nprocname > sizeof("httpd:") -1 */
    strcpy(config->procname, "httpd:");
    config->procname  += sizeof("httpd:")-1;
    config->nprocname -= sizeof("httpd:")-1;
    memset(config->procname, 0x00, config->nprocname);
}

/* [starttime ][host][uri][?args][ headers] */

static size_t wayd_set_starttime(wayd_server_config_t *config, size_t n)
{
    struct tm ltm;
    time_t now = time(0);
    n += strftime(config->procname, config->nprocname, "%H:%M:%S ",
            localtime_r(&now, &ltm));
    return n;
}

static size_t wayd_set_host(request_rec *r, wayd_server_config_t *config, size_t n)
{
    const char *host = apr_table_get(r->headers_in, "Host");
    if (host) {
        strncpy(config->procname + n, host, config->nprocname - n);
        n += strlen(host);
    }
    return n;
}

static size_t wayd_set_uri(request_rec *r, wayd_server_config_t *config, size_t n)
{
    size_t nuri = strlen(r->uri);
    if (nuri > config->urilimit) nuri = config->urilimit;
    size_t min  = (config->nprocname - n < nuri) ? (config->nprocname - n) : nuri;
    strncpy(config->procname + n, r->uri, min);
    n += min;

    return n;
}

static size_t wayd_set_args(request_rec *r, wayd_server_config_t *config, size_t n)
{
    size_t nargs = strlen(r->args);
    if (nargs > config->argslimit) nargs = config->argslimit;

    if (config->nprocname - n > 0) {
        config->procname[n++] = '?';
    }

    size_t min = (config->nprocname - n < nargs) ? (config->nprocname - n) : nargs;
    strncpy(config->procname + n, r->args, min);
    n += min;

    return n;
}

static size_t wayd_set_headers(request_rec *r, wayd_server_config_t *config, size_t n)
{
    int i = 0;
    char **elts = (char **) config->headers->elts;
    for (i = 0; i < config->headers->nelts; ++i) {
        const char *header = apr_table_get(r->headers_in, elts[i]);
        if (header) {
            size_t nheader = strlen(header);

            if (config->nprocname - n > 0) {
                config->procname[n++] = ' ';
            }

            size_t min = (config->nprocname - n < nheader) ? (config->nprocname - n) : nheader;
            strncpy(config->procname + n, header, min);
            n += min;
        }
    }

    return n;
}

/*
 * This routine is called after the request has been read but before any other
 * phases have been processed.  This allows us to make decisions based upon
 * the input header fields.
 *
 * The return value is OK, DECLINED, or HTTP_mumble.  If we return OK, no
 * further modules are called for this phase.
 */
static int wayd_post_read_request(request_rec *r)
{
    // if we are in the internal redirect or we are in subrequest
    if (r->prev || r->main) {
        return DECLINED; 
    }

    wayd_server_config_t *config;
    config = ap_get_module_config(r->server->module_config, &wayd_module);

    size_t n = 0;
    if (config->starttime) {
        n = wayd_set_starttime(config, n);
    }

    if (config->host) {
        n = wayd_set_host(r, config, n);
    }

    if (config->uri && r->uri) {
        n = wayd_set_uri(r, config, n);
    }

    if (config->args && r->args) {
        n = wayd_set_args(r, config, n);
    }

    if (config->headers) {
        n = wayd_set_headers(r, config, n);
    }

    return DECLINED;
}

static int wayd_clean_reqinfo(request_rec *r)
{
    wayd_server_config_t *config;
    config = ap_get_module_config(r->server->module_config, &wayd_module);

    memset(config->procname, 0x00, config->nprocname);
    return OK;
}

/*--------------------------------------------------------------------------*/
/*                                                                          */
/* Which functions are responsible for which hooks in the server.           */
/*                                                                          */
/*--------------------------------------------------------------------------*/

static void wayd_register_hooks(apr_pool_t *p)
{
    ap_hook_child_init(wayd_child_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_read_request(wayd_post_read_request, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_log_transaction(wayd_clean_reqinfo, NULL, NULL, APR_HOOK_MIDDLE);
}

static const char *wayd_add_headers(cmd_parms *cmd, void *dummy, int argc, char *const argv[])
{
    wayd_server_config_t *config;
    config = ap_get_module_config(cmd->server->module_config, &wayd_module);

    if (!config->headers) {
        config->headers = apr_array_make(cmd->pool, 10, sizeof(char*));
    }

    int i;
    for (i = 0; i < argc; ++i) {
        char **ele = apr_array_push(config->headers);
        *ele = apr_pstrdup(cmd->pool, argv[i]);
    }

    return NULL;
}

static const char *wayd_set_boolean_slot(cmd_parms *cmd, void *dummy, const char *arg)
{
    int   offset     = (int)(long)cmd->info;
    char *config_ptr = (char *) ap_get_module_config(cmd->server->module_config, &wayd_module);

    if (strcasecmp(arg, "On") == 0) {
        *(int *)(config_ptr + offset) = 1;
    } else if (strcasecmp(arg, "Off") == 0) {
        *(int *)(config_ptr + offset) = 0;
    } else {
        return "use one of: off | on";
    }

    return NULL;
}

static const char *wayd_set_int_slot(cmd_parms *cmd, void *dummy, const char *arg)
{
    int   offset     = (int)(long)cmd->info;
    char *config_ptr = (char *) ap_get_module_config(cmd->server->module_config, &wayd_module);

    *(int *)(config_ptr + offset) = atoi(arg);

    return NULL;
}

/*--------------------------------------------------------------------------*/
/*                                                                          */
/* All of the routines have been declared now.  Here's the list of          */
/* directives specific to our module, and information about where they      */
/* may appear and how the command parser should pass them to us for         */
/* processing.  Note that care must be taken to ensure that there are NO    */
/* collisions of directive names between modules.                           */
/*                                                                          */
/*--------------------------------------------------------------------------*/

static const command_rec wayd_cmds[] =
{
    AP_INIT_TAKE1("WaydStarttime", wayd_set_boolean_slot,
            (void *)APR_OFFSETOF(wayd_server_config_t, starttime), RSRC_CONF,
            "show starttime or not, default no"),

    AP_INIT_TAKE1("WaydHost", wayd_set_boolean_slot,
            (void *)APR_OFFSETOF(wayd_server_config_t, host), RSRC_CONF,
            "show host or not, default no"),

    AP_INIT_TAKE1("WaydUri", wayd_set_boolean_slot,
            (void *)APR_OFFSETOF(wayd_server_config_t, uri), RSRC_CONF,
            "show uri or not, default yes"),

    AP_INIT_TAKE1("WaydUriSizeLimit", wayd_set_int_slot,
            (void *)APR_OFFSETOF(wayd_server_config_t, urilimit), RSRC_CONF,
            "uri length limit, default 64"),

    AP_INIT_TAKE1("WaydArgs", wayd_set_boolean_slot,
            (void *)APR_OFFSETOF(wayd_server_config_t, args), RSRC_CONF,
            "show query string or not, default no"),

    AP_INIT_TAKE1("WaydArgsSizeLimit", wayd_set_int_slot,
            (void *)APR_OFFSETOF(wayd_server_config_t, argslimit), RSRC_CONF,
            "query string length limit, default 64"),

    AP_INIT_TAKE_ARGV("WaydHeaders", wayd_add_headers,
            NULL, RSRC_CONF,
            "show headers"),

    { NULL }
};

/*--------------------------------------------------------------------------*/
/*                                                                          */
/* Finally, the list of callback routines and data structures that provide  */
/* the static hooks into our module from the other parts of the server.     */
/*                                                                          */
/*--------------------------------------------------------------------------*/

module AP_MODULE_DECLARE_DATA wayd_module =
{
    STANDARD20_MODULE_STUFF,
    NULL,                         /* per-directory config creator */
    NULL,                         /* dir config merger */
    wayd_create_server_config,    /* server config creator */
    NULL,                         /* server config merger */
    wayd_cmds,                    /* command table */
    wayd_register_hooks,          /* set up other request processing hooks */
};
