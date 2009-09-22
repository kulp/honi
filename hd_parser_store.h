/*
 *
 */

#include <stddef.h>

/**
 * A callback function that returns a pointer to data starting at @p offset and
 * continuing for @p count bytes.
 */
typedef const char* (*chunker_t)(void *userdata, unsigned long offset, size_t count);

typedef int (*hd_filestore_init)(struct hd_parser_state *state, void *data);
typedef int (*hd_filestore_fini)(struct hd_parser_state *state);

extern chunker_t hd_get_chunker(struct hd_parser_state *state);
extern int hd_set_chunker(struct hd_parser_state *state, chunker_t chunker);

/* vim:set et ts=4 sw=4 syntax=c.doxygen: */

