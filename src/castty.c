#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "castty.h"
#include "edit.h"
#include "record.h"

static void
usage(int status)
{

	fprintf(stderr, "usage: castty <command> [options]\n"
	    " record    Create a new recording. See castty record -h for\n"
	    "           options specific to recording.\n"
	    " edit      Inspect and edit an existing recording. See\n"
	    "           castty edit -h for options specific to editing.\n");

	exit(status);
}

int
main(int argc, char **argv)
{

	setup_sighandlers();

	if (argc < 2) {
		usage(EXIT_FAILURE);
	}

	argc--;
	argv++;

	if (strcmp(argv[0], "record") == 0) {
		return record_main(argc, argv);
	} else if (strcmp(argv[0], "edit") == 0) {
		return edit_main(argc, argv);
	} else {
		usage(EXIT_FAILURE);
	}
}
