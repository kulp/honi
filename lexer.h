#ifndef LEXER_H_
#define LEXER_H_

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define _err(...) do { \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
} while (0)

/**
 * A callback function that returns a pointer to data starting at @p offset and
 * continuing for @p count bytes.
 */
typedef const char* (*chunker_t)(void *userdata, unsigned long offset, size_t count);

#endif /* LEXER_H_ */

/* vim:set ts=4 sw=4 syntax=c.doxygen: */

