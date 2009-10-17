#include "hd_parser.h"
#include "hd_parser_store.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syck.h>
#include <unistd.h>
#include <yaml.h>

#define _err(...) do { \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
} while (0)

struct hd_parser_state {
    chunker_t chunker;
    void *userdata;
};

struct hd_node {
    enum type {
        NODE_HASH   = 'a',
        NODE_BOOL   = 'b',
        NODE_INT    = 'i',
        NODE_STRING = 's',
        NODE_NULL   = 'N',
    } type;
    union nodeval {
        struct hash {
            long len;
            struct hashval {
                hd_node *key;
                hd_node *val;
            } *pairs;
        } a;
        bool b;
        long long i;
        struct {
            long  len;
            char *val;
        } s;
    } val;
};

static int compare_pairs(const void *a, const void *b)
{
    int rc = 0;

    const hd_node *f = *(hd_node **)a;
    const hd_node *s = *(hd_node **)b;

    /// @todo support comparison of hash types ?
    // sort types separately (not mutually comparable usually anyway)
    rc = f->type - s->type;
    if (!rc && f->type == NODE_STRING)
        rc = strcmp(f->val.s.val, s->val.s.val);

    return rc;
}

static hd_node *hd_dispatch(struct hd_parser_state *state, int *pos);

static hd_node *hd_handle_bool(struct hd_parser_state *state, int *pos)
{
    hd_node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 3);
    if (!input)
        return NULL;

    char *next;
    int intval = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return HD_PARSE_FAILURE;
    }

    result = malloc(sizeof *result);
    *result = (hd_node){ .type = NODE_BOOL, .val = { .b = intval } };
    (*pos) += next - input;

    return result;
}

static hd_node *hd_handle_hash(struct hd_parser_state *state, int *pos)
{
    hd_node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 10);
    if (!input)
        return NULL;

    char *next;
    int len = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return HD_PARSE_FAILURE;
    }

    int inc = next - input + 2; // 2 for ":{"
    (*pos) += inc;
    input = state->chunker(state->userdata, *pos, inc);
    if (!input)
        return NULL;

    struct hashval *pairs = malloc(len * sizeof *pairs);
    for (int i = 0; i < len; i++) {
        pairs[i].key = hd_dispatch(state, pos);
        if (!pairs[i].key) return NULL;
        pairs[i].val = hd_dispatch(state, pos);
        if (!pairs[i].val) return NULL;
    }

    // putting entries in order allows bsearch() on them
    qsort(pairs, len, sizeof *pairs, compare_pairs);

    result = malloc(sizeof *result);
    *result = (hd_node){
        .type = NODE_HASH,
        .val = { .a = { .len = len, .pairs = pairs } },
    };
    (*pos) += 1; // 1 for the closing brace

    return result;
}

static hd_node *hd_handle_string(struct hd_parser_state *state, int *pos)
{
    hd_node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 10);
    if (!input)
        return NULL;

    char *next;
    int len = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return HD_PARSE_FAILURE;
    }

    (*pos) += next - input + 2; // 1 for colon, 1 for opening quote
    input = state->chunker(state->userdata, *pos, len);
    if (!input)
        return NULL;

    char *val = malloc(len + 1);
    /// @todo what about possibly escaped characters ?
    strncpy(val, next + 2, len);
    val[len] = 0;

    result = malloc(sizeof *result);
    *result = (hd_node){
        .type = NODE_STRING,
        .val = { .s = { .len = len, .val = val } }
    };

    (*pos) += len + 1;  // 1 for closing quote

    return result;
}

static hd_node *hd_handle_int(struct hd_parser_state *state, int *pos)
{
    hd_node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 10);
    if (!input)
        return NULL;

    char *next;
    long intval = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return HD_PARSE_FAILURE;
    }

    result = malloc(sizeof *result);
    *result = (hd_node){ .type = NODE_INT, .val = { .i = intval } };
    (*pos) += next - input;

    return result;
}

static hd_node *hd_handle_null(struct hd_parser_state *state, int *pos)
{
    hd_node *result = NULL;

    result = malloc(sizeof *result);
    *result = (hd_node){ .type = NODE_NULL };
    (*pos) += 1; // 'N'

    return result;
}

static hd_node *hd_dispatch(struct hd_parser_state *state, int *pos)
{
    hd_node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 1);
    if (!input)
        return NULL;

    int here = 0;
    while (isspace(input[here]))
        here++;

    (*pos) += here;

    switch (input[here]) {
        case NODE_BOOL:   result = hd_handle_bool  (state, pos); break;
        case NODE_HASH:   result = hd_handle_hash  (state, pos); break;
        case NODE_STRING: result = hd_handle_string(state, pos); break;
        case NODE_INT:    result = hd_handle_int   (state, pos); break;
        case NODE_NULL:   result = hd_handle_null  (state, pos); break;

        case '}':
        case ';': (*pos)++; result = hd_dispatch(state, pos); break;

        default:
            _err("Parse failure in %s", __func__);
            return HD_PARSE_FAILURE;
    }

    return result;
}

static int _hd_dump_recurse(FILE *f, const hd_node *node, int level, int flags)
{
    int rc = 0;

    char spaces[(level + 1) * HD_INDENT_SIZE + 1];
    memset(spaces, ' ', sizeof spaces - 1);
    spaces[sizeof spaces - 1] = 0;

    char less[level * HD_INDENT_SIZE + 1];
    memset(less, ' ', sizeof less - 1);
    less[sizeof less - 1] = 0;

    const union nodeval *v = &node->val;
    bool pretty = flags & HD_PRINT_PRETTY;

    switch (node->type) {
        case NODE_STRING: fprintf(f, "s:%ld:\"%s\"", v->s.len, v->s.val); break;
        case NODE_BOOL  : fprintf(f, "b:%u"        , v->b);               break;
        case NODE_INT   : fprintf(f, "i:%lld"      , v->i);               break;
        case NODE_NULL  : fprintf(f, "N");                                break;
        case NODE_HASH  :
            fprintf(f, "a:%ld:{%s", v->a.len, pretty ? "\n" : "");
            for (int i = 0; i < v->a.len; i++) {
                if (pretty)
                    fputs(spaces, f);
                rc = _hd_dump_recurse(f, v->a.pairs[i].key, level + 1, flags);
                fputs(";", f);
                rc = _hd_dump_recurse(f, v->a.pairs[i].val, level + 1, flags);
                // this is a hack (inconsistent format / space-saver -- feature
                // or bug, depending on your point of view) -- not my idea !
                if (i != v->a.len - 1 && v->a.pairs[i].val->type != NODE_HASH)
                    fputs(";", f);
                if (pretty)
                    fputs("\n", f);
            }

            if (pretty)
                fputs(less, f);
            fputs("}", f);
            break;
        default: return -1;
    }

    return rc;
}

static int node_emitter(yaml_emitter_t *e, const hd_node *node)
{
    int rc = 0;

    enum { NONE, SCALAR, COLLECTION } mode = NONE;

    yaml_event_t event;

    char *what;
    char tempbuf[20];
    int len;
    switch (node->type) {
        case NODE_INT:
            mode = SCALAR;
            len  = sprintf(what = tempbuf, "%lld", node->val.i);
            break;
        case NODE_BOOL:
            mode = SCALAR;
            what = node->val.b ? "true" : "false";
            len  = strlen(what);
            break;
        case NODE_STRING:
            mode = SCALAR;
            what = node->val.s.val;
            len  = node->val.s.len;
            break;
        case NODE_NULL:
            mode = SCALAR;
            what = "~";
            len = 1;
            break;
        case NODE_HASH:
            mode = COLLECTION;
            yaml_mapping_start_event_initialize(&event, NULL, NULL, 0,
                    YAML_BLOCK_MAPPING_STYLE);
            yaml_emitter_emit(e, &event);

            for (int i = 0; i < node->val.a.len; i++) {
                if (!rc) rc = node_emitter(e, node->val.a.pairs[i].key);
                if (!rc) rc = node_emitter(e, node->val.a.pairs[i].val);
            }

            yaml_mapping_end_event_initialize(&event);
            yaml_emitter_emit(e, &event);
            break;
        default:
            _err("Unrecognized node type '%d'", node->type);
            return -1;
    }

    if (mode == SCALAR) {
        yaml_scalar_event_initialize(&event, NULL, NULL, (yaml_char_t*)what,
                len, true, true, YAML_PLAIN_SCALAR_STYLE);
        yaml_emitter_emit(e, &event);
    }

    return rc;
}

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------

int hd_init(struct hd_parser_state **state)
{
    if (!state) return -1;
    return !!(*state = malloc(sizeof **state));
}

int hd_fini(struct hd_parser_state **state)
{
    if (!state) return -1;
    free(*state);
    *state = NULL;
    return 0;
}

void* hd_get_userdata(struct hd_parser_state *state)
{
    return state->userdata;
}

int hd_set_userdata(struct hd_parser_state *state, void *data)
{
    state->userdata = data;
    return 0;
}

chunker_t hd_get_chunker(struct hd_parser_state *state)
{
    return state->chunker;
}

int hd_set_chunker(struct hd_parser_state *state, chunker_t chunker)
{
    state->chunker = chunker;
    return 0;
}

hd_node *hd_parse(struct hd_parser_state *state)
{
    int pos = 0;
    return hd_dispatch(state, &pos);
}

int hd_yaml(FILE *f, const hd_node *node, int flags)
{
    int rc = 0;

    yaml_emitter_t emitter;
    yaml_event_t event;

    yaml_emitter_initialize(&emitter);

    yaml_emitter_set_output_file(&emitter, f);

    yaml_stream_start_event_initialize(&event, YAML_UTF8_ENCODING);
    if (!yaml_emitter_emit(&emitter, &event))
        goto error;

    yaml_document_start_event_initialize(&event, NULL, NULL, NULL, 0);
    if (!yaml_emitter_emit(&emitter, &event))
        goto error;

    rc = node_emitter(&emitter, node);

    yaml_document_end_event_initialize(&event, 1);
    if (!yaml_emitter_emit(&emitter, &event))
        goto error;

    yaml_stream_end_event_initialize(&event);
    if (!yaml_emitter_emit(&emitter, &event))
        goto error;

done:
    yaml_emitter_delete(&emitter);
    return rc;

error:
    rc = -1;

    goto done;
}

int hd_dump(FILE *f, const hd_node *node, int flags)
{
    int rc = 0;

    rc = _hd_dump_recurse(f, node, 0, flags);
    fputs("\n", f);

    return rc;
}

void hd_free(hd_node* node)
{
    switch (node->type) {
        case NODE_BOOL   : 
        case NODE_INT    : 
        case NODE_NULL   : break;
        case NODE_STRING : free(node->val.s.val); break;
        case NODE_HASH   :
            for (int i = 0; i < node->val.a.len; i++) {
                hd_free(node->val.a.pairs[i].key);
                hd_free(node->val.a.pairs[i].val);
            }
            free(node->val.a.pairs);
            break;
        default:
            _err("Invalid node type %d in %s", node->type, __func__);
    };

    free(node);
}

/* vim:set et ts=4 sw=4 syntax=c.doxygen: */

