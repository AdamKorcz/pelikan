#include <slimcache/data/process.h>

#include <protocol/data/memcache_include.h>
#include <storage/cuckoo/cuckoo.h>

#include <cc_array.h>
#include <cc_debug.h>
#include <cc_print.h>

#define SLIMCACHE_PROCESS_MODULE_NAME "slimcache::process"

#define STORE_ERR_MSG "invalid/oversized value, cannot be stored"
#define DELTA_ERR_MSG "value is not a number"
#define OTHER_ERR_MSG "command not supported"

static bool process_init = false;
static process_metrics_st *process_metrics = NULL;
static bool allow_flush = ALLOW_FLUSH;

void
process_setup(process_options_st *options, process_metrics_st *metrics)
{
    log_info("set up the %s module", SLIMCACHE_PROCESS_MODULE_NAME);
    if (process_init) {
        log_warn("%s has already been setup, overwrite",
                SLIMCACHE_PROCESS_MODULE_NAME);
    }

    process_metrics = metrics;

    if (options != NULL) {
        allow_flush = option_bool(&options->allow_flush);
    }

    process_init = true;
}

void
process_teardown(void)
{
    log_info("tear down the %s module", SLIMCACHE_PROCESS_MODULE_NAME);
    if (!process_init) {
        log_warn("%s has never been setup", SLIMCACHE_PROCESS_MODULE_NAME);
    }

    process_metrics = NULL;
    process_init = false;
    allow_flush = false;
}


static bool
_get_key(struct response *rsp, struct bstring *key)
{
    struct item *it;
    struct val val;

    it = cuckoo_get(key);
    if (it != NULL) {
        rsp->type = RSP_VALUE;
        rsp->key = *key;
        rsp->flag = item_flag(it);
        rsp->vcas = item_cas(it);
        item_val(&val, it);
        if (val.type == VAL_TYPE_INT) {
            rsp->num = 1;
            rsp->vint = val.vint;
        } else {
            rsp->vstr = val.vstr;
        }

        log_verb("found key at %p, location %p", key, it);
        return true;
    } else {
        log_verb("key at %p not found", key);
        return false;
    }
}

static void
_process_get(struct response *rsp, struct request *req)
{
    struct bstring *key;
    struct response *r = rsp;
    uint32_t i;

    INCR(process_metrics, get);
    /* use chained responses, move to the next response if key is found. */
    for (i = 0; i < array_nelem(req->keys); ++i) {
        INCR(process_metrics, get_key);
        key = array_get(req->keys, i);
        if (_get_key(r, key)) {
            r->cas = false;
            r = STAILQ_NEXT(r, next);
            if (r == NULL) {
                INCR(process_metrics, get_ex);
                log_warn("get response incomplete due to lack of rsp objects");
                return;
            }
            req->nfound++;
            INCR(process_metrics, get_key_hit);
        } else {
            INCR(process_metrics, get_key_miss);
        }
    }
    r->type = RSP_END;

    log_verb("get req %p processed, %d out of %d keys found", req, req->nfound, i);
}

static void
_process_gets(struct response *rsp, struct request *req)
{
    struct bstring *key;
    struct response *r = rsp;
    uint32_t i;

    INCR(process_metrics, gets);
    /* use chained responses, move to the next response if key is found. */
    for (i = 0; i < array_nelem(req->keys); ++i) {
        INCR(process_metrics, gets_key);
        key = array_get(req->keys, i);
        if (_get_key(r, key)) {
            r->cas = true;
            r = STAILQ_NEXT(r, next);
            if (r == NULL) {
                INCR(process_metrics, gets_ex);
                log_warn("gets response incomplete due to lack of rsp objects");
            }
            req->nfound++;
            INCR(process_metrics, gets_key_hit);
        } else {
            INCR(process_metrics, gets_key_miss);
        }
    }
    r->type = RSP_END;

    log_verb("gets req %p processed, %d out of %d keys found", req, req->nfound, i);
}

static void
_process_delete(struct response *rsp, struct request *req)
{
    INCR(process_metrics, delete);
    if (cuckoo_delete(array_first(req->keys))) {
        rsp->type = RSP_DELETED;
        INCR(process_metrics, delete_deleted);
    } else {
        rsp->type = RSP_NOT_FOUND;
        INCR(process_metrics, delete_notfound);
    }

    log_verb("delete req %p processed, rsp type %d", req, rsp->type);
}

static void
_get_value(struct val *val, struct bstring *vstr)
{
    rstatus_i status;

    log_verb("processing value at %p, store at %p", vstr, val);

    status = bstring_atou64(&val->vint, vstr);
    if (status == CC_OK) {
        val->type = VAL_TYPE_INT;
    } else {
        val->type = VAL_TYPE_STR;
        val->vstr = *vstr;
    }
}

static inline void
_error_rsp(struct response *rsp, char *msg)
{
    INCR(process_metrics, process_ex);
    rsp->type = RSP_CLIENT_ERROR;
    rsp->vstr = str2bstr(msg);
}

static void
_process_set(struct response *rsp, struct request *req)
{
    rstatus_i status = CC_OK;
    rel_time_t expire;
    struct bstring *key;
    struct item *it;
    struct val val;

    INCR(process_metrics, set);
    key = array_first(req->keys);
    expire = time_reltime(req->expiry);
    _get_value(&val, &req->vstr);

    it = cuckoo_get(key);
    if (it != NULL) {
        status = cuckoo_update(it, &val, expire);
    } else {
        status = cuckoo_insert(key, &val, expire);
    }

    if (status == CC_OK) {
        rsp->type = RSP_STORED;
        INCR(process_metrics, set_stored);
    } else {
        _error_rsp(rsp, STORE_ERR_MSG);
        INCR(process_metrics, set_ex);
    }

    log_verb("set req %p processed, rsp type %d", req, rsp->type);
}

static void
_process_add(struct response *rsp, struct request *req)
{
    struct bstring *key;
    struct item *it;
    struct val val;

    INCR(process_metrics, add);
    key = array_first(req->keys);
    it = cuckoo_get(key);
    if (it != NULL) {
        rsp->type = RSP_NOT_STORED;
        INCR(process_metrics, add_notstored);
    } else {
        _get_value(&val, &req->vstr);
        if (cuckoo_insert(key, &val, time_reltime(req->expiry)) == CC_OK) {
            rsp->type = RSP_STORED;
            INCR(process_metrics, add_stored);
        } else {
            _error_rsp(rsp, STORE_ERR_MSG);
            INCR(process_metrics, add_ex);
        }
    }

    log_verb("add req %p processed, rsp type %d", req, rsp->type);
}

static void
_process_replace(struct response *rsp, struct request *req)
{
    struct bstring *key;
    struct item *it;
    struct val val;

    INCR(process_metrics, replace);
    key = array_first(req->keys);
    it = cuckoo_get(key);
    if (it != NULL) {
        _get_value(&val, &req->vstr);
        if (cuckoo_update(it, &val, time_reltime(req->expiry)) == CC_OK) {
            rsp->type = RSP_STORED;
            INCR(process_metrics, replace_stored);
        } else {
            _error_rsp(rsp, STORE_ERR_MSG);
            INCR(process_metrics, replace_ex);
        }
    } else {
        rsp->type = RSP_NOT_STORED;
        INCR(process_metrics, replace_notstored);
    }

    log_verb("replace req %p processed, rsp type %d", req, rsp->type);
}

static void
_process_cas(struct response *rsp, struct request *req)
{
    struct bstring *key;
    struct item *it;
    struct val val;

    INCR(process_metrics, cas);
    key = array_first(req->keys);
    it = cuckoo_get(key);
    if (it != NULL) {

        if (item_cas_valid(it, req->vcas)) {
            _get_value(&val, &req->vstr);
            if (cuckoo_update(it, &val, time_reltime(req->expiry)) == CC_OK) {
                rsp->type = RSP_STORED;
                INCR(process_metrics, cas_stored);
            } else {
                _error_rsp(rsp, STORE_ERR_MSG);
                INCR(process_metrics, cas_ex);
            }
        } else {
            rsp->type = RSP_EXISTS;
            INCR(process_metrics, cas_exists);
        }
    } else {
        rsp->type = RSP_NOT_FOUND;
        INCR(process_metrics, cas_notfound);
    }

    log_verb("cas req %p processed, rsp type %d", req, rsp->type);
}

static void
_process_incr(struct response *rsp, struct request *req)
{
    struct bstring *key;
    struct item *it;
    struct val nval;

    INCR(process_metrics, incr);
    key = array_first(req->keys);
    it = cuckoo_get(key);
    if (NULL != it) {
        if (item_vtype(it) != VAL_TYPE_INT) {
            _error_rsp(rsp, DELTA_ERR_MSG);
            INCR(process_metrics, incr_ex);
            /* TODO(yao): binary key */
            log_warn("value not int, cannot apply incr on key %.*s val %.*s",
                    key->len, key->data, it->vlen, ITEM_VAL_POS(it));
            return;
        }

        nval.type = VAL_TYPE_INT;
        nval.vint = item_value_int(it) + req->delta;
        item_value_update(it, &nval);
        rsp->type = RSP_NUMERIC;
        rsp->vint = nval.vint;
        INCR(process_metrics, incr_stored);
    } else {
        rsp->type = RSP_NOT_FOUND;
        INCR(process_metrics, incr_notfound);
    }

    log_verb("incr req %p processed, rsp type %d", req, rsp->type);
}

static void
_process_decr(struct response *rsp, struct request *req)
{
    struct bstring *key;
    struct item *it;
    uint64_t v;
    struct val nval;

    INCR(process_metrics, decr);
    key = array_first(req->keys);
    it = cuckoo_get(key);
    if (NULL != it) {
        if (item_vtype(it) != VAL_TYPE_INT) {
            _error_rsp(rsp, DELTA_ERR_MSG);
            INCR(process_metrics, decr_ex);
            /* TODO(yao): binary key */
            log_warn("value not int, cannot apply decr on key %.*s val %.*s",
                    key->len, key->data, it->vlen, ITEM_VAL_POS(it));
            return;
        }

        v = item_value_int(it);
        nval.type = VAL_TYPE_INT;
        if (v < req->delta) {
            nval.vint = 0;
        } else {
            nval.vint = v - req->delta;
        }
        item_value_update(it, &nval);
        rsp->type = RSP_NUMERIC;
        rsp->vint = nval.vint;
        INCR(process_metrics, decr_stored);
    } else {
        rsp->type = RSP_NOT_FOUND;
        INCR(process_metrics, decr_notfound);
    }

    log_verb("incr req %p processed, rsp type %d", req, rsp->type);
}

static void
_process_flush(struct response *rsp, struct request *req)
{
    if (allow_flush) {
        INCR(process_metrics, flush);
        cuckoo_reset();
        rsp->type = RSP_OK;

        log_info("flush req %p processed, rsp type %d", req, rsp->type);
    } else {
        _error_rsp(rsp, OTHER_ERR_MSG);
    }
}

void
process_request(struct response *rsp, struct request *req)
{
    log_verb("processing req %p, write rsp to %p", req, rsp);
    INCR(process_metrics, process_req);

    switch (req->type) {
    case REQ_GET:
        _process_get(rsp, req);
        break;

    case REQ_GETS:
        _process_gets(rsp, req);
        break;

    case REQ_DELETE:
        _process_delete(rsp, req);
        break;

    case REQ_SET:
        _process_set(rsp, req);
        break;

    case REQ_ADD:
        _process_add(rsp, req);
        break;

    case REQ_REPLACE:
        _process_replace(rsp, req);
        break;

    case REQ_CAS:
        _process_cas(rsp, req);
        break;

    case REQ_INCR:
        _process_incr(rsp, req);
        break;

    case REQ_DECR:
        _process_decr(rsp, req);
        break;

    case REQ_FLUSH:
        _process_flush(rsp, req);
        break;

    default:
        rsp->type = RSP_CLIENT_ERROR;
        rsp->vstr = str2bstr(OTHER_ERR_MSG);
        break;
    }
}
