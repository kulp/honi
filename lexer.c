#include "filestore.h"
#include "lexer.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <syck.h>
#include <fcntl.h>

#define PARSE_FAILURE ((void*)-1)

struct lexer_state {
    chunker_t chunker;
    void *userdata;
    int out_fd;
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
static inline void* _alloc(size_t size)
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
    if (!rc) {
        if (f->type == NODE_STRING)
            rc = strcmp(f->val.s.val, s->val.s.val);
    }

    return rc;
}

struct node *hd_dispatch(struct lexer_state *state, int *pos);

struct node *hd_handle_bool(struct lexer_state *state, int *pos)
{
    struct node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 3);

    char *next;
    int intval = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return PARSE_FAILURE;
    }

    result = _alloc(sizeof *result);
    *result = (struct node){ .type = NODE_BOOL, .val = { .b = intval } };
    (*pos) += next - input;

    return result;
}

struct node *hd_handle_hash(struct lexer_state *state, int *pos)
{
    struct node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 10);

    char *next;
    int len = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return PARSE_FAILURE;
    }

    int inc = next - input + 2; // 2 for ":}"
    (*pos) += inc;
    input = state->chunker(state->userdata, *pos, inc);

    struct hashval *pairs = _alloc(len * sizeof *pairs);
    for (int i = 0; i < len; i++) {
        pairs[i].key = hd_dispatch(state, pos);
        pairs[i].val = hd_dispatch(state, pos);
    }

    // putting entries in order allows bsearch() on them
    qsort(pairs, len, sizeof *pairs, compare_pairs);

    result = _alloc(sizeof *result);
    *result = (struct node){
        .type = NODE_HASH,
        .val = { .a = { .len = len, .pairs = pairs } },
    };
    (*pos) += 1; // 1 for the closing brace

    return result;
}

struct node *hd_handle_string(struct lexer_state *state, int *pos)
{
    struct node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 10);

    char *next;
    int len = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return PARSE_FAILURE;
    }

    (*pos) += next - input + 2; // 1 for colon, 1 for opening quote
    input = state->chunker(state->userdata, *pos, len);

    char *val = _alloc(len + 1);
    /// @todo what about possibly escaped characters ?
    strncpy(val, next + 2, len);
    val[len] = 0;

    result = _alloc(sizeof *result);
    *result = (struct node){
        .type = NODE_STRING,
        .val = { .s = { .len = len, .val = val } }
    };

    (*pos) += len + 1;  // 1 for closing quote

    return result;
}

struct node *hd_handle_int(struct lexer_state *state, int *pos)
{
    struct node *result = NULL;

    const char *input = state->chunker(state->userdata, *pos, 10);

    char *next;
    long intval = strtol(&input[2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return PARSE_FAILURE;
    }

    result = _alloc(sizeof *result);
    *result = (struct node){ .type = NODE_INT, .val = { .i = intval } };
    (*pos) += next - input;

    return result;
}

struct node *hd_handle_null(struct lexer_state *state, int *pos)
{
    struct node *result = NULL;

    result = _alloc(sizeof *result);
    *result = (struct node){ .type = NODE_NULL };
    (*pos) += 1; // 'N'

    return result;
}

struct node *hd_dispatch(struct lexer_state *state, int *pos)
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

        default: break;
    }

    return result;
}

#define INDENT_SIZE 4
static int _hd_dump_recursive(const struct node *node, int level)
{
    int rc = 0;

    char spaces[(level + 1) * INDENT_SIZE + 1];
    memset(spaces, ' ', sizeof spaces - 1);
    spaces[sizeof spaces - 1] = 0;

    char less[level * INDENT_SIZE + 1];
    memset(less, ' ', sizeof less - 1);
    less[sizeof less - 1] = 0;

    switch (node->type) {
        case NODE_STRING: printf("s:%ld:\"%s\"", node->val.s.len, node->val.s.val); break;
        case NODE_BOOL  : printf("b:%d"        , node->val.b);                      break;
        case NODE_INT   : printf("i:%ld"       , node->val.i);                      break;
        case NODE_NULL  : printf("N");                                              break;
        case NODE_HASH  :
            printf("a:%ld:{\n", node->val.a.len);
            for (int i = 0; i < node->val.a.len; i++) {
                fputs(spaces, stdout);
                rc = _hd_dump_recursive(node->val.a.pairs[i].key, level + 1);
                fputc(';', stdout);
                rc = _hd_dump_recursive(node->val.a.pairs[i].val, level + 1);
                // this is a hack (inconsistent format / space-saver -- feature
                // or bug, depending on your point of view) -- not my idea !
                if (i != node->val.a.len - 1 && node->val.a.pairs[i].val->type != NODE_HASH)
                    fputc(';', stdout);
                fputs("\n", stdout);
            }

            fputs(less, stdout);
            fputs("}", stdout);
            break;
        default: return -1;
    }

    return rc;
}

int hd_dump(const struct node *node)
{
    int rc = 0;

    rc = _hd_dump_recursive(node, 0);
    fputc('\n', stdout);

    return rc;
}

static void output_handler(SyckEmitter *e, char *ptr, long len)
{
    const struct lexer_state *state = e->bonus;
    write(state->out_fd, ptr, len);
}

static void emitter_handler(SyckEmitter *e, st_data_t data)
{
    const struct node *node = (const struct node *)data;

    enum { NONE, SCALAR, COLLECTION } mode = NONE;

    char *what;
    char tempbuf[10];
    int len;
    switch (node->type) {
        case NODE_INT   :
            mode = SCALAR;
            len  = sprintf(what = tempbuf, "%ld", node->val.i);
            break;
        case NODE_BOOL  :
            mode = SCALAR;
            what = node->val.b ? "true" : "false";
            len  = strlen(what);
            break;
        case NODE_STRING:
            mode = SCALAR;
            what = node->val.s.val;
            len  = node->val.s.len;
            break;
        case NODE_NULL  :
            mode = SCALAR;
            what = NULL;
            len = 0;
            break;
        case NODE_HASH  :
            mode = COLLECTION;
            syck_emit_map(e, NULL);

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
        syck_emit_scalar(e, NULL, scalar_plain, 1, 1, 1, what, len);
    // if it's a COLLECTION, it has already been emitted
}

int hd_yaml(const struct lexer_state *state, const struct node *node)
{
    int rc = 0;

    SyckEmitter *e = syck_new_emitter();
    e->bonus = (void*)state;

    syck_output_handler (e, output_handler);
    syck_emitter_handler(e, emitter_handler);

    syck_emit(e, (st_data_t)node);
    syck_emitter_flush(e, 0);

    syck_free_emitter(e);

    return rc;
}

int main(int argc, char *argv[])
{
    int rc = 0;

    struct lexer_state state;

    if (argc < 2 || argc > 3) {
        _err("Supply an input filename and an optional output filename");
        return EXIT_FAILURE;
    }

    rc = hd_read_file_init(argv[1], &state.chunker, &state.userdata);
    if (rc) return EXIT_FAILURE;

    if (argc == 2) {
        state.out_fd = fileno(stdout);
    } else {
        state.out_fd = open(argv[2], O_WRONLY | O_CREAT);
        if (state.out_fd < 0) {
            _err("Failed to open output file '%s'", argv[2]);
            return EXIT_FAILURE;
        }
    }

    int pos = 0;
    struct node *result = hd_dispatch(&state, &pos);

    //rc = hd_dump(result);
    rc = hd_yaml(&state, result);

    rc = hd_read_file_fini(state.userdata);

    close(state.out_fd);

    return rc;
}

/* vim:set ts=4 sw=4 syntax=c.doxygen: */

