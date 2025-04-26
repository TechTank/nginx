#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>
#include <ngx_stream_script.h>


//-----------------------------------------------------------------------------
// main (global) configuration

typedef struct {
    ngx_flag_t  enable;          // global on/off switch
    size_t      max_inject_len;  // maximum length of evaluated inject string
} ngx_stream_inject_main_conf_t;


static void *
ngx_stream_inject_create_main_conf(ngx_conf_t *cf)
{
    ngx_stream_inject_main_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) {
        return NULL;
    }

    conf->enable          = NGX_CONF_UNSET;
    conf->max_inject_len  = NGX_CONF_UNSET_SIZE;
    return conf;
}


static char *
ngx_stream_inject_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_stream_inject_main_conf_t  *mcf = conf;

    ngx_conf_init_value(mcf->enable, 1);
    ngx_conf_init_size_value(mcf->max_inject_len, 1024);

    return NGX_CONF_OK;
}


//-----------------------------------------------------------------------------
// per-server configuration

typedef struct {
    ngx_stream_complex_value_t  inject;         // compiled "complex" value
    ngx_flag_t                  has_variables; // "$…" detected at compile time
} ngx_stream_inject_srv_conf_t;


static void *
ngx_stream_inject_create_srv_conf(ngx_conf_t *cf)
{
    return ngx_pcalloc(cf->pool, sizeof(ngx_stream_inject_srv_conf_t));
}


static char *
ngx_stream_inject_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_stream_inject_srv_conf_t *prev = parent;
    ngx_stream_inject_srv_conf_t *conf = child;

    // inherit compiled value & flag if unset in child
    if (conf->inject.value.data == NULL) {
        conf->inject        = prev->inject;
        conf->has_variables = prev->has_variables;
    }

    return NGX_CONF_OK;
}


//-----------------------------------------------------------------------------
// directives

static char *
ngx_stream_inject_set_string(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_inject_main_conf_t   *mcf;
    ngx_stream_inject_srv_conf_t    *iscf = conf;
    ngx_stream_compile_complex_value_t  ccv;
    ngx_str_t                       *value;
    ngx_uint_t                      i;

    mcf   = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_inject_module);
    value = cf->args->elts;

    // check for null bytes
    for (i = 0; i < value[1].len; i++) {
        if (value[1].data[i] == '\0') {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "inject_string contains null byte");
            return NGX_CONF_ERROR;
        }
    }

    // enforce global max length on literal
    if ((size_t) value[1].len > mcf->max_inject_len) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "inject_string too long (%ui > %z)",
                           value[1].len, mcf->max_inject_len);
        return NGX_CONF_ERROR;
    }

    // compile as complex value (supports $remote_addr, etc.)
    ngx_memzero(&ccv, sizeof(ccv));
    ccv.cf            = cf;
    ccv.value         = &value[1];
    ccv.complex_value = &iscf->inject;

    if (ngx_stream_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    // mark if we saw any “$” for runtime evaluation
    iscf->has_variables = (ngx_strchr(value[1].data, '$') != NULL);

    return NGX_CONF_OK;
}


static ngx_command_t  ngx_stream_inject_commands[] = {

    { ngx_string("inject_enable"),
      NGX_STREAM_MAIN_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_STREAM_MAIN_CONF_OFFSET,
      offsetof(ngx_stream_inject_main_conf_t, enable),
      NULL },

    { ngx_string("inject_max_length"),
      NGX_STREAM_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_STREAM_MAIN_CONF_OFFSET,
      offsetof(ngx_stream_inject_main_conf_t, max_inject_len),
      NULL },

    { ngx_string("inject_string"),
      NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
      ngx_stream_inject_set_string,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    ngx_null_command
};


//-----------------------------------------------------------------------------
// preread handler

static ngx_int_t
ngx_stream_inject_preread(ngx_stream_session_t *s,
                          ngx_stream_phase_handler_t *ph)
{
    ngx_stream_inject_main_conf_t  *mcf;
    ngx_stream_inject_srv_conf_t   *iscf;
    ngx_connection_t               *c;
    ngx_str_t                       inject;
    ngx_buf_t                      *b;
    ngx_chain_t                    *cl;
    ngx_int_t                       rc;

    mcf  = ngx_stream_conf_get_module_main_conf(s, ngx_stream_inject_module);
    if (mcf->enable == 0) {
        return NGX_OK;
    }

    iscf = ngx_stream_get_module_srv_conf(s, ngx_stream_inject_module);

    // nothing to inject?
    if (iscf->inject.value.len == 0) {
        return NGX_OK;
    }

    // evaluate variables if needed
    if (iscf->has_variables) {
        if (ngx_stream_complex_value(s, &iscf->inject, &inject) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "stream_inject: variable evaluation failed");
            return NGX_ERROR;
        }
    } else {
        inject = iscf->inject.value;
    }

    // enforce length on evaluated string
    if (inject.len > mcf->max_inject_len) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "stream_inject: evaluated string too long (%uz > %z)",
                      inject.len, mcf->max_inject_len);
        return NGX_ERROR;
    }

    // debug log the final payload
    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream_inject: sending %V", &inject);

    // build a single buffer chain
    b = ngx_create_temp_buf(s->connection->pool, inject.len);
    if (b == NULL) {
        return NGX_ERROR;
    }
    b->last = ngx_cpymem(b->pos, inject.data, inject.len);
    b->last_buf = 1;

    cl = ngx_alloc_chain_link(s->connection->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }
    cl->buf = b;
    cl->next = NULL;

    // send via stream write filter (handles non-blocking & backpressure)
    rc = ngx_stream_write_filter(s, cl);

    if (rc == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "stream_inject: write filter failed");
        return NGX_ERROR;
    }

    return rc;  // NGX_OK or NGX_AGAIN
}


//-----------------------------------------------------------------------------
// postconfiguration: register preread handler

static ngx_int_t
ngx_stream_inject_init(ngx_conf_t *cf)
{
    ngx_stream_core_main_conf_t  *cmcf;
    ngx_stream_handler_pt        *h;

    cmcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_STREAM_PREREAD_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_stream_inject_preread;
    return NGX_OK;
}


//-----------------------------------------------------------------------------
// module context & definition

static ngx_stream_module_t  ngx_stream_inject_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_stream_inject_init,                /* postconfiguration */

    ngx_stream_inject_create_main_conf,    /* create main configuration */
    ngx_stream_inject_init_main_conf,      /* init main configuration */

    ngx_stream_inject_create_srv_conf,     /* create server configuration */
    ngx_stream_inject_merge_srv_conf       /* merge server configuration */
};


ngx_module_t ngx_stream_inject_module = {
    NGX_MODULE_V1,
    &ngx_stream_inject_module_ctx,     /* module context */
    ngx_stream_inject_commands,        /* directives */
    NGX_STREAM_MODULE,                 /* module type */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};