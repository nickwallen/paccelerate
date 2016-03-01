#include "args.h"

/*
 * Print usage information to the user.
 */
void print_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK\n"
			"  -p PORTMASK: hexadecimal bitmask of ports to configure\n",
			prgname);
}

/*
 * Parse the 'portmask' command line argument.
 */
int parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	// parse hexadecimal string
	pm = strtoul(portmask, &end, 16);

	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0')) {
    return -1;
  } else if (pm == 0) {
    return -1;
  } else {
    return pm;
  }
}

/**
 * Parse the command line arguments passed to the application.
 */
int parse_args(int argc, char **argv)
{
	int opt;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];
	static struct option lgopts[] = {
		{ NULL, 0, 0, 0 }
	};

  // parse arguments to this application
	argvopt = argv;
	while ((opt = getopt_long(argc, argvopt, "p:", lgopts, &option_index)) != EOF) {
		switch (opt) {

		// portmask
		case 'p':
			app.enabled_port_mask = parse_portmask(optarg);
			if (app.enabled_port_mask == 0) {
				printf("Error: Invalid portmask: '%s'\n", optarg);
				print_usage(prgname);
				return -1;
			}
			break;

		default:
      printf("Error: Invalid argument: '%s'\n", optarg);
			print_usage(prgname);
			return -1;
		}
	}

	if (optind <= 1) {
		print_usage(prgname);
		return -1;
	}

	argv[optind-1] = prgname;

  // reset getopt lib
	optind = 0;
	return 0;
}
