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

struct node {
    enum type {
        NODE_HASH   = 'a',
        NODE_BOOL   = 'b',
        NODE_INT    = 'i',
        NODE_STRING = 's',
        NODE_NULL   = 'N',
    } type;
    union {
        struct hash {
            long len;
            struct hashval {
                struct node *key;
                struct node *val;
            } *pairs;
        } a;
        bool b;
        long i;
        struct {
            long  len;
            char *val;
        } s;
    } val;
};

/// @todo write a more efficient chunking allocator
static void* hd_alloc(size_t size)
{
    return malloc(size);
}

static int compare_pairs(const void *a, const void *b)
{
    int rc = 0;

    const struct node *f = *(struct node **)a;
    const struct node *s = *(struct node **)b;

    /// @todo support comparison of hash types ?
    // sort types separately (not mutually comparable usually anyway)
    rc = f->type - s->type;
    if (!rc && f->type == NODE_STRING)
        rc = strcmp(f->val.s.val, s->val.s.val);

    return rc;
}

static struct node *hd_dispatch(struct hd_parser_state *state, int *pos);

static struct node *hd_handle_bool(struct hd_parser_state *state, int *pos)
{
    struct node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 3);

    char *next;
    int intval = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return HD_PARSE_FAILURE;
    }

    result = hd_alloc(sizeof *result);
    *result = (struct node){ .type = NODE_BOOL, .val = { .b = intval } };
    (*pos) += next - input;

    return result;
}

static struct node *hd_handle_hash(struct hd_parser_state *state, int *pos)
{
    struct node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 10);

    char *next;
    int len = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return HD_PARSE_FAILURE;
    }

    int inc = next - input + 2; // 2 for ":}"
    (*pos) += inc;
    input = state->chunker(state->userdata, *pos, inc);

    struct hashval *pairs = hd_alloc(len * sizeof *pairs);
    for (int i = 0; i < len; i++) {
        pairs[i].key = hd_dispatch(state, pos);
        pairs[i].val = hd_dispatch(state, pos);
    }

    // putting entries in order allows bsearch() on them
    qsort(pairs, len, sizeof *pairs, compare_pairs);

    result = hd_alloc(sizeof *result);
    *result = (struct node){
        .type = NODE_HASH,
        .val = { .a = { .len = len, .pairs = pairs } },
    };
    (*pos) += 1; // 1 for the closing brace

    return result;
}

static struct node *hd_handle_string(struct hd_parser_state *state, int *pos)
{
    struct node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 10);

    char *next;
    int len = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return HD_PARSE_FAILURE;
    }

    (*pos) += next - input + 2; // 1 for colon, 1 for opening quote
    input = state->chunker(state->userdata, *pos, len);

    char *val = hd_alloc(len + 1);
    /// @todo what about possibly escaped characters ?
    strncpy(val, next + 2, len);
    val[len] = 0;

    result = hd_alloc(sizeof *result);
    *result = (struct node){
        .type = NODE_STRING,
        .val = { .s = { .len = len, .val = val } }
    };

    (*pos) += len + 1;  // 1 for closing quote

    return result;
}

static struct node *hd_handle_int(struct hd_parser_state *state, int *pos)
{
    struct node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 10);

    char *next;
    long intval = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return HD_PARSE_FAILURE;
    }

    result = hd_alloc(sizeof *result);
    *result = (struct node){ .type = NODE_INT, .val = { .i = intval } };
    (*pos) += next - input;

    return result;
}

static struct node *hd_handle_null(struct hd_parser_state *state, int *pos)
{
    struct node *result = NULL;

    result = hd_alloc(sizeof *result);
    *result = (struct node){ .type = NODE_NULL };
    (*pos) += 1; // 'N'

    return result;
}

static struct node *hd_dispatch(struct hd_parser_state *state, int *pos)
{
    struct node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 1);

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

/// portable version of GNU dprintf()
static int _fdprintf(int fd, const char *fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    int size = vsnprintf(NULL, 0, fmt, vl);
    va_end(vl);
    va_start(vl, fmt);
    char buf[size + 1];
    int result = vsnprintf(buf, size + 1, fmt, vl);
    write(fd, buf, size);
    va_end(vl);
    return result;
}

#define INDENT_SIZE 4
static int _hd_dump_recursive(int fd, const struct node *node, int level, int flags)
{
    int rc = 0;

    char spaces[(level + 1) * INDENT_SIZE + 1];
    memset(spaces, ' ', sizeof spaces - 1);
    spaces[sizeof spaces - 1] = 0;

    char less[level * INDENT_SIZE + 1];
    memset(less, ' ', sizeof less - 1);
    less[sizeof less - 1] = 0;

    switch (node->type) {
        case NODE_STRING: _fdprintf(fd, "s:%ld:\"%s\"", node->val.s.len, node->val.s.val); break;
        case NODE_BOOL  : _fdprintf(fd, "b:%d"        , node->val.b);                      break;
        case NODE_INT   : _fdprintf(fd, "i:%ld"       , node->val.i);                      break;
        case NODE_NULL  : _fdprintf(fd, "N");                                              break;
        case NODE_HASH  :
            _fdprintf(fd, "a:%ld:{%s", node->val.a.len, flags & HD_PRINT_PRETTY ? "\n" : "");
            for (int i = 0; i < node->val.a.len; i++) {
                if (flags & HD_PRINT_PRETTY)
                    write(fd, spaces, sizeof spaces - 1);
                rc = _hd_dump_recursive(fd, node->val.a.pairs[i].key, level + 1, flags);
                write(fd, ";", 1);
                rc = _hd_dump_recursive(fd, node->val.a.pairs[i].val, level + 1, flags);
                // this is a hack (inconsistent format / space-saver -- feature
                // or bug, depending on your point of view) -- not my idea !
                if (i != node->val.a.len - 1 && node->val.a.pairs[i].val->type != NODE_HASH)
                    write(fd, ";", 1);
                if (flags & HD_PRINT_PRETTY)
                    write(fd, "\n", 1);
            }

            if (flags & HD_PRINT_PRETTY)
                write(fd, less, sizeof less - 1);
            write(fd, "}", 1);
            break;
        default: return -1;
    }

    return rc;
}

static void output_handler(SyckEmitter *e, char *ptr, long len)
{
    write((uintptr_t)e->bonus, ptr, len);
}

static int write_handler(void *ext, unsigned char *buffer, size_t size)
{
    return write((uintptr_t)ext, buffer, size) > 0;
}

static void emitter_handler(SyckEmitter *e, st_data_t data)
{
    const struct node *node = (const struct node *)data;

    enum { NONE, SCALAR, COLLECTION } mode = NONE;

    char *what;
    char tempbuf[10];
    int len;
    switch (node->type) {
        case NODE_INT:
            mode = SCALAR;
            len  = sprintf(what = tempbuf, "%ld", node->val.i);
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
            what = NULL;
            len = 0;
            break;
        case NODE_HASH:
            mode = COLLECTION;
            syck_emit_map(e, NULL, map_none);

            for (int i = 0; i < node->val.a.len; i++) {
                syck_emit_item(e, (st_data_t)node->val.a.pairs[i].key);
                syck_emit_item(e, (st_data_t)node->val.a.pairs[i].val);
            }

            syck_emit_end(e);
            break;
        default:
            _err("Unrecognized node type '%d'", node->type);
            return;
    }

    if (mode == SCALAR)
        syck_emit_scalar(e, NULL, scalar_none, 1, 1, 1, what, len); // if it's a COLLECTION, it has already been emitted
}

static void node_emitter(yaml_emitter_t *e, const struct node *node)
{
    enum { NONE, SCALAR, COLLECTION } mode = NONE;

    yaml_event_t event;
    yaml_scalar_style_t style = YAML_PLAIN_SCALAR_STYLE;

    char *what;
    char tempbuf[10];
    int len;
    switch (node->type) {
        case NODE_INT:
            mode = SCALAR;
            len  = sprintf(what = tempbuf, "%ld", node->val.i);
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
            what = "null";
            len = 4;
            break;
        case NODE_HASH:
            mode = COLLECTION;
            yaml_mapping_start_event_initialize(&event, NULL, NULL, 0, YAML_BLOCK_MAPPING_STYLE); /// @todo 1 ?
            yaml_emitter_emit(e, &event);

            for (int i = 0; i < node->val.a.len; i++) {
                node_emitter(e, node->val.a.pairs[i].key);
                node_emitter(e, node->val.a.pairs[i].val);
            }

            yaml_mapping_end_event_initialize(&event);
            yaml_emitter_emit(e, &event);
            break;
        default:
            _err("Unrecognized node type '%d'", node->type);
            return;
    }

    if (mode == SCALAR) {
        yaml_scalar_event_initialize(&event, NULL, NULL, (yaml_char_t*)what, len, true, true, style);
        yaml_emitter_emit(e, &event);
    }
}

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------

int hd_init(struct hd_parser_state **state)
{
    *state = hd_alloc(sizeof **state);
    return !!state;
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

struct node *hd_parse(struct hd_parser_state *state)
{
    int pos = 0;
    return hd_dispatch(state, &pos);
}

int hd_yaml(int fd, const struct node *node, int flags)
{
    int rc = 0;

    /// @todo use @p flags

    SyckEmitter *e = syck_new_emitter();
    e->bonus = (void*)(uintptr_t)fd;

    syck_output_handler (e, output_handler);
    syck_emitter_handler(e, emitter_handler);

    syck_emit(e, (st_data_t)node);
    syck_emitter_flush(e, 0);

    syck_free_emitter(e);

    return rc;
}

int hd_yaml2(int fd, const struct node *node, int flags)
{
    int rc = 0;

    yaml_emitter_t emitter;
    yaml_event_t event;

    yaml_emitter_initialize(&emitter);

    yaml_emitter_set_output(&emitter, write_handler, (void*)(uintptr_t)fd);

    /* Create and emit the STREAM-START event. */
    yaml_stream_start_event_initialize(&event, YAML_UTF8_ENCODING);
    if (!yaml_emitter_emit(&emitter, &event))
        goto error;

    yaml_document_start_event_initialize(&event, NULL, NULL, NULL, 0);
    if (!yaml_emitter_emit(&emitter, &event))
        goto error;

    node_emitter(&emitter, node);

    yaml_document_end_event_initialize(&event, 1);
    if (!yaml_emitter_emit(&emitter, &event))
        goto error;

    yaml_stream_end_event_initialize(&event);
    if (!yaml_emitter_emit(&emitter, &event))
        goto error;

error:
    yaml_emitter_delete(&emitter);

    return rc;
}

int hd_dump(int fd, const struct node *node, int flags)
{
    int rc = 0;

    rc = _hd_dump_recursive(fd, node, 0, flags);
    write(fd, "\n", 1);

    return rc;
}

void hd_free(struct node* node)
{
    switch (node->type) {
        case NODE_BOOL   : 
        case NODE_INT    : 
        case NODE_NULL   :                           break;
        case NODE_STRING : free(node->val.s.val);    break;
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

