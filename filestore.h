#ifndef FILESTORE_H_
#define FILESTORE_H_

#include "hd_parser.h"
#include "hd_parser_store.h"

int hd_read_file_init(struct hd_parser_state *state, void *data);
int hd_read_file_fini(struct hd_parser_state *state);

#endif /* FILESTORE_H_ */

