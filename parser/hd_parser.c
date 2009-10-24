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
        NODE_ARRAY  = 'a',
        NODE_BOOL   = 'b',
        NODE_INT    = 'i',
        NODE_FLOAT  = 'd',
        NODE_NULL   = 'N',
        NODE_OBJECT = 'O',
        NODE_STRING = 's',
    } type;
    union nodeval {
        struct array {
            long len;
            struct arrayval {
                hd_node *key;
                hd_node *val;
            } *pairs;
        } a;
        bool b;
        long double d;
        long long i;
        struct object {
            char *type;
            struct array val;
        } o;
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

    /// @todo support comparison of array types ?
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

static hd_node * _hd_object_or_hash(struct hd_parser_state *state, int *pos, int len, struct array *where)
{
    struct arrayval *pairs = malloc(len * sizeof *pairs);
    for (int i = 0; i < len; i++) {
        pairs[i].key = hd_dispatch(state, pos);
        if (!pairs[i].key) return NULL;
        pairs[i].val = hd_dispatch(state, pos);
        if (!pairs[i].val) return NULL;
    }

    // putting entries in order allows bsearch() on them
    qsort(pairs, len, sizeof *pairs, compare_pairs);

    where->len   = len;
    where->pairs = pairs;
    (*pos) += 1; // 1 for the closing brace

    return NULL;
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

    result = malloc(sizeof *result);
    if (_hd_object_or_hash(state, pos, len, &result->val.a) == HD_PARSE_FAILURE)
        return HD_PARSE_FAILURE;
    result->type = NODE_ARRAY;

    return result;
}

static hd_node *hd_handle_float(struct hd_parser_state *state, int *pos)
{
    hd_node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 10);
    if (!input)
        return NULL;

    char *next;
    long double floatval = strtold(&input[2], &next);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return HD_PARSE_FAILURE;
    }

    result = malloc(sizeof *result);
    *result = (hd_node){ .type = NODE_FLOAT, .val = { .d = floatval } };
    (*pos) += next - input;

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

static hd_node *hd_handle_object(struct hd_parser_state *state, int *pos)
{
    hd_node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 10);
    if (!input) return HD_PARSE_FAILURE;

    char *next;
    int typelen = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return HD_PARSE_FAILURE;
    }

    (*pos) += next - input;;
    // +2 for quotes
    input = state->chunker(state->userdata, *pos, typelen + 2);
    if (!input) return HD_PARSE_FAILURE;

    char type[typelen + 1];
    memcpy(type, &input[2], typelen);
    type[typelen] = 0;
    (*pos) += typelen + 4;
    input = state->chunker(state->userdata, *pos, 10);
    if (!input) return HD_PARSE_FAILURE;

    int len = strtol(input, &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return HD_PARSE_FAILURE;
    }

    (*pos) += next - input + 2; // 2 for ":{"

    result = malloc(sizeof *result);
    if (_hd_object_or_hash(state, pos, len, &result->val.o.val) == HD_PARSE_FAILURE)
        return HD_PARSE_FAILURE;
    result->type = NODE_OBJECT;
    result->val.o.type = strdup(type);

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
        case NODE_ARRAY:  result = hd_handle_hash  (state, pos); break;
        case NODE_BOOL:   result = hd_handle_bool  (state, pos); break;
        case NODE_FLOAT:  result = hd_handle_float (state, pos); break;
        case NODE_INT:    result = hd_handle_int   (state, pos); break;
        case NODE_NULL:   result = hd_handle_null  (state, pos); break;
        case NODE_OBJECT: result = hd_handle_object(state, pos); break;
        case NODE_STRING: result = hd_handle_string(state, pos); break;

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

    const struct array *what = NULL;
    switch (node->type) {
        case NODE_STRING: fprintf(f, "s:%ld:\"%s\"", v->s.len, v->s.val); break;
        case NODE_BOOL  : fprintf(f, "b:%u"        , v->b);               break;
        case NODE_INT   : fprintf(f, "i:%lld"      , v->i);               break;
        case NODE_NULL  : fprintf(f, "N");                                break;
        case NODE_OBJECT:
            what = &node->val.o.val;
            fprintf(f, "O:%u:\"%s\":%ld:{%s", strlen(v->o.type), v->o.type, v->o.val.len, pretty ? "\n" : "");
            goto inside_array;
        case NODE_ARRAY :
            if (!what) what = &node->val.a;
            fprintf(f, "a:%ld:{%s", v->a.len, pretty ? "\n" : "");
        inside_array:
            for (int i = 0; i < what->len; i++) {
                if (pretty)
                    fputs(spaces, f);
                rc = _hd_dump_recurse(f, what->pairs[i].key, level + 1, flags);
                fputs(";", f);
                rc = _hd_dump_recurse(f, what->pairs[i].val, level + 1, flags);
                // this is a hack (inconsistent format / space-saver -- feature
                // or bug, depending on your point of view) -- not my idea !
                if (i != what->len - 1 && what->pairs[i].val->type != NODE_ARRAY)
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

    yaml_char_t *tag = NULL;
    const struct array *where = NULL;
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
        case NODE_OBJECT:
            tag = (yaml_char_t*)node->val.o.type;
            where = &node->val.o.val;
            goto inside_array;
        case NODE_ARRAY:
            where = &node->val.a;
        inside_array:
            mode = COLLECTION;
            yaml_mapping_start_event_initialize(&event, NULL, tag, 0,
                    YAML_BLOCK_MAPPING_STYLE);
            yaml_emitter_emit(e, &event);

            for (int i = 0; i < where->len; i++) {
                if (!rc) rc = node_emitter(e, where->pairs[i].key);
                if (!rc) rc = node_emitter(e, where->pairs[i].val);
            }

            yaml_mapping_end_event_initialize(&event);
            yaml_emitter_emit(e, &event);
            break;
        default:
            _err("Unrecognized node type '%c' (%d)", node->type, node->type);
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
    struct array *what = NULL;

    switch (node->type) {
        case NODE_BOOL   : 
        case NODE_INT    : 
        case NODE_NULL   : break;
        case NODE_STRING : free(node->val.s.val); break;
        case NODE_OBJECT :
            what = &node->val.o.val;
            free(node->val.o.type);
            goto inside_array;
        case NODE_ARRAY  :
            what = &node->val.a;
        inside_array:
            for (int i = 0; i < what->len; i++) {
                hd_free(what->pairs[i].key);
                hd_free(what->pairs[i].val);
            }
            free(what->pairs);
            break;
        default:
            _err("Invalid node type %d in %s", node->type, __func__);
    };

    free(node);
}

/* vim:set et ts=4 sw=4 syntax=c.doxygen: */

