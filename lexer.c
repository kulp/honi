#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define _err(...) do { \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
} while (0)

#define PARSE_FAILURE ((void*)-1)

struct lexer_state {
    /// @todo abstract away the data-getting details; add a function pointer
    /// here to fetch next data with, and add an implementation that uses files,
    /// as well as one that uses a given buffer
    FILE *file;
    long start;
    long stop;
    struct stat stat;
    char buf[BUFSIZ];
};

struct node {
    enum type {
        NODE_HASH   = 'a',
        NODE_BOOL   = 'b',
        NODE_INT    = 'i',
        NODE_STRING = 's',
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

int read_file(struct lexer_state *state, const char *filename)
{
    int rc = 0;

    state->file = fopen(filename, "r");
    if (!state->file) {
        _err("File '%s' could not be opened (%d: %s)",
             filename, errno, strerror(errno));
        return -1;
    }

    rc = fstat(fileno(state->file), &state->stat);
    if (rc) {
        _err("fstat: %d: %s", errno, strerror(errno));
        return rc;
    }

    state->start = state->stop = 0;

    /// @todo read whole file in to start; later, read only a bit at a time
    state->start += fread(state->buf, 1, state->stop += sizeof state->buf, state->file);

    return rc;
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

struct node *dispatch(const char *input, int *pos);

struct node *handle_bool(const char *input, int *pos)
{
    struct node *result = NULL;

    char *next;
    int intval = strtol(&input[*pos + 2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return PARSE_FAILURE;
    }

    result = _alloc(sizeof *result);
    *result = (struct node){ .type = NODE_BOOL, .val = { .b = intval } };
    (*pos) += next - (input + *pos);

    return result;
}

struct node *handle_hash(const char *input, int *pos)
{
    struct node *result = NULL;

    char *next;
    int len = strtol(&input[*pos + 2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return PARSE_FAILURE;
    }

    (*pos) += next - (input + *pos) + 2; // 2 for ":}"

    struct hashval *pairs = _alloc(len * sizeof *pairs);
    for (int i = 0; i < len; i++) {
        pairs[i].key = dispatch(input, pos);
        pairs[i].val = dispatch(input, pos);
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

struct node *handle_string(const char *input, int *pos)
{
    struct node *result = NULL;

    char *next;
    int len = strtol(&input[*pos + 2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return PARSE_FAILURE;
    }

    char *val = _alloc(len + 1);
    /// @todo what about possibly escaped characters ?
    strncpy(val, next + 2, len);
    val[len] = 0;

    result = _alloc(sizeof *result);
    *result = (struct node){
        .type = NODE_STRING,
        .val = { .s = { .len = len, .val = val } }
    };

    // 1 for colon, 2 for quotes
    (*pos) += next - (input + *pos) + len + 1 + 2;

    return result;
}

struct node *handle_int(const char *input, int *pos)
{
    struct node *result = NULL;

    char *next;
    long intval = strtol(&input[*pos + 2], &next, 10);
    if (next == input) {
        _err("Parse failure in %s", __func__);
        return PARSE_FAILURE;
    }

    result = _alloc(sizeof *result);
    *result = (struct node){ .type = NODE_INT, .val = { .i = intval } };
    (*pos) += next - (input + *pos);

    return result;
}

struct node *dispatch(const char *input, int *pos)
{
    struct node *result = NULL;

    switch (input[*pos]) {
        case NODE_BOOL:   result = handle_bool  (input, pos); break;
        case NODE_HASH:   result = handle_hash  (input, pos); break;
        case NODE_STRING: result = handle_string(input, pos); break;
        case NODE_INT:    result = handle_int   (input, pos); break;

        case '}':
        case ';': (*pos)++; result = dispatch(input, pos); break;

        default: break;
    }

    return result;
}

int main(int argc, char *argv[])
{
    int rc = 0;

    struct lexer_state state;

    if (argc != 2) {
        _err("Supply a filename");
        return EXIT_FAILURE;
    }

    rc = read_file(&state, argv[1]);
    int pos = 0;
    struct node *result = dispatch(state.buf, &pos);

    return rc;
}

/* vim:set ts=4 sw=4 syntax=c.doxygen: */

