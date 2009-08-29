#include "filestore.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct filestate {
    int fd;
    char *data;
    struct stat stat;
};

static const char* _chunker(void *data, unsigned long offset, size_t count)
{
    struct filestate *state = data;

    return &state->data[offset];
}

int hd_read_file_fini(void *data)
{
    struct filestate *state = data;

    if (!state) return -1;

    munmap(state->data, state->stat.st_size);
    close(state->fd);
    free(state);

    return 0;
}

int hd_read_file_init(const char *filename, chunker_t *chunker, void **data)
{
    int rc = 0;

    struct filestate *state = malloc(sizeof *state);

    state->fd = open(filename, O_RDONLY);
    if (state->fd < 0) {
        _err("File '%s' could not be opened (%d: %s)",
             filename, errno, strerror(errno));
        return -1;
    }

    rc = fstat(state->fd, &state->stat);
    if (rc) {
        _err("fstat: %d: %s", errno, strerror(errno));
        return rc;
    }

    state->data = mmap(NULL, state->stat.st_size, PROT_READ, MAP_PRIVATE, state->fd, 0);

    if (data   ) *data    = state   ; else return -1;
    if (chunker) *chunker = _chunker; else return -1;

    return rc;
}

/* vim:set ts=4 sw=4 syntax=c.doxygen: */

