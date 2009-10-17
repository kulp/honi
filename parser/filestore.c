#include "filestore.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define _err(...) do { \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
} while (0)

struct filestate {
    FILE *f;
    char *data;
    size_t len;
    struct stat stat;
    size_t start, end;
};

static const char* _chunker(void *data, unsigned long offset, size_t count)
{
    struct filestate *state = data;

    /*
     * This code could be optimized, as it might read the same data more than
     * once. However, we know that the particular usage of this program will
     * read data unidirectionally in small chunks, so optimization and / or
     * generalization are probably unwarranted.
     */

    if (offset < state->start || offset + count >= state->end) {
        if (count > state->len) {
            state->data = realloc(state->data, state->len = count * 2);
        }

        fseek(state->f, state->start = offset, SEEK_SET);
        state->end = state->start + fread(state->data, 1, state->len, state->f);
    }

    return &state->data[offset - state->start];
}

int hd_read_file_init(struct hd_parser_state *parser_state, void *userdata)
{
    int rc = 0;

    const char *filename = userdata;

    struct filestate *state = malloc(sizeof *state);

    state->f = fopen(filename, "r");
    if (!state->f) {
        _err("File '%s' could not be opened (%d: %s)",
             filename, errno, strerror(errno));
        return -1;
    }

    rc = stat(filename, &state->stat);
    if (rc) {
        _err("stat: %d: %s", errno, strerror(errno));
        return rc;
    }

    state->len = BUFSIZ;
    state->data = malloc(state->len);
    state->start = state->end = 0;

    hd_set_userdata(parser_state, state);
    hd_set_chunker(parser_state, _chunker);

    return rc;
}

int hd_read_file_fini(struct hd_parser_state *parser_state)
{
    struct filestate *state = hd_get_userdata(parser_state);

    if (!state) return -1;

    fclose(state->f);
    free(state);

    return 0;
}

/* vim:set ts=4 sw=4 syntax=c.doxygen: */

