#ifndef FILESTORE_H_
#define FILESTORE_H_

#include "lexer.h"

int hd_read_file_init(const char *filename, chunker_t *chunker, void **data);
int hd_read_file_fini(void *data);

#endif /* FILESTORE_H_ */

