/**
 * Converts a file in HoNData hierarchical format to YAML.
 */

#include "hd_parser.h"
#include "filestore.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    int rc = 0;

    struct hd_parser_state *state;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Supply an input filename and an optional output filename\n");
        return EXIT_FAILURE;
    }

    rc = hd_init(&state);
    rc = hd_read_file_init(state, argv[1]);
    if (rc) return EXIT_FAILURE;

    int fd = -1;
    if (argc == 2) {
        fd = fileno(stdout);
    } else {
        fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            fprintf(stderr, "Failed to open output file '%s'\n", argv[2]);
            return EXIT_FAILURE;
        }
    }

    struct node *result = hd_parse(state);
    rc = hd_yaml(fd, result, 0);
    rc = hd_read_file_fini(state);
    rc = hd_fini(&state);
    hd_free(result);

    return rc;
}

/* vim:set et ts=4 sw=4 syntax=c.doxygen: */

