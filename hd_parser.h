#ifndef HD_PARSER_H_
#define HD_PARSER_H_

#include <stdbool.h>
#include <stddef.h>

struct hd_parser_state;

#define HD_PRINT_PRETTY 1

/**
 * Called to initialize an opaque HoNData parser state.
 *
 * @param state a pointer to a state pointer
 *
 * @return zero on success, non-zero on undifferentiated failure
 */
int hd_init(struct hd_parser_state **state);

/**
 * Called to finalize an opaque HoNData parser state. Frees all internal data
 * associated with the state, but not any data returned by hd_parse().
 *
 * @param state a pointer to a state pointer
 *
 * @return zero on success, non-zero on undifferentiated failure.
 */
int hd_fini(struct hd_parser_state **state);

/** @defgroup getset Getters and setters for state internals */
/** @{ */
void* hd_get_userdata(struct hd_parser_state *state);
int hd_set_userdata(struct hd_parser_state *state, void *data);
/** @} */

/**
 * Parses the set-up store and returns the resulting tree.
 *
 * @param state the parser state
 *
 * @return a @c node or @c NULL on undifferentiated error
 */
struct node *hd_parse(struct hd_parser_state *state);

/**
 * Dumps a particular tree or subtree, as YAML, to a file descriptor @p fd.
 *
 * @param fd   the file descriptor to which to dump
 * @param node the root of the (sub)tree to dump
 * @param flags OR'ed flags that control the output
 *
 * @return zero on success, non-zero on undifferentiated error
 */
int hd_yaml(int fd, const struct node *node, int flags);

/**
 * Dumps a particular tree or subtree, in the native format, to the file
 * descriptor @p fd.
 *
 * @param fd    the file descriptor to which to dump
 * @param node  the root of the (sub)tree to dump
 * @param flags OR'ed flags that control the output
 *
 * @return zero on success, non-zero on undifferentiated error
 */
int hd_dump(int fd, const struct node *node, int flags);

void hd_free(struct node* ptr);

#endif /* HD_PARSER_H_ */

/* vim:set ts=4 sw=4 syntax=c.doxygen: */

