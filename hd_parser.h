#ifndef HD_PARSER_H_
#define HD_PARSER_H_

struct hd_parser_state;

int hd_init(struct hd_parser_state **state);
int hd_fini(struct hd_parser_state **state);
int hd_get_out_fd(struct hd_parser_state *state);
int hd_set_out_fd(struct hd_parser_state *state, int out_fd);
void* hd_get_userdata(struct hd_parser_state *state);
int hd_set_userdata(struct hd_parser_state *state, void *data);
struct node *hd_parse(struct hd_parser_state *state);
int hd_yaml(const struct hd_parser_state *state, const struct node *node);
int hd_dump(const struct node *node);

#endif /* HD_PARSER_H_ */

/* vim:set ts=4 sw=4 syntax=c.doxygen: */

