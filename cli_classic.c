/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2000 Silicon Integrated System Corporation
 * Copyright (C) 2004 Tyan Corp <yhlu@tyan.com>
 * Copyright (C) 2005-2008 coresystems GmbH
 * Copyright (C) 2008,2009,2010 Carl-Daniel Hailfinger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <getopt.h>
#include "flash.h"
#include "flashchips.h"
#include "fmap.h"
#include "programmer.h"
#include "libflashprog.h"

static void cli_classic_usage(const char *name)
{
	printf("Usage: %s [-h|-R|-L|"
#if CONFIG_PRINT_WIKI == 1
	       "-z|"
#endif
	       "\n\t-p <programmername>[:<parameters>] [-c <chipname>]\n"
	       "\t\t(--flash-name|--flash-size|\n"
	       "\t\t [-E|(-r|-w|-v) <file>]\n"
	       "\t\t [(-l <layoutfile>|--ifd| --fmap|--fmap-file <file>) [-i <imagename>]...]\n"
	       "\t\t [-n] [-N] [-f])]\n"
	       "\t[-V[V[V]]] [-o <logfile>]\n\n", name);

	printf(" -h | --help                        print this help text\n"
	       " -R | --version                     print version (release)\n"
	       " -r | --read <file>                 read flash and save to <file>\n"
	       " -w | --write (<file>|-)            write <file> or the content provided\n"
	       "                                    on the standard input to flash\n"
	       " -v | --verify (<file>|-)           verify flash against <file>\n"
	       "                                    or the content provided on the standard input\n"
	       " -E | --erase                       erase flash memory\n"
	       " -V | --verbose                     more verbose output\n"
	       " -c | --chip <chipname>             probe only for specified flash chip\n"
	       " -f | --force                       force specific operations (see man page)\n"
	       " -n | --noverify                    don't auto-verify\n"
	       " -N | --noverify-all                verify included regions only (cf. -i)\n"
	       " -l | --layout <layoutfile>         read ROM layout from <layoutfile>\n"
	       "      --flash-name                  read out the detected flash name\n"
	       "      --flash-size                  read out the detected flash size\n"
	       "      --fmap                        read ROM layout from fmap embedded in ROM\n"
	       "      --fmap-file <fmapfile>        read ROM layout from fmap in <fmapfile>\n"
	       "      --ifd                         read layout from an Intel Firmware Descriptor\n"
	       " -i | --include <region>            only read/write image <region> from layout\n"
	       "      --image <region>              deprecated, please use --include\n"
	       " -o | --output <logfile>            log output to <logfile>\n"
	       "      --flash-contents <ref-file>   assume flash contents to be <ref-file>\n"
	       " -L | --list-supported              print supported devices\n"
#if CONFIG_PRINT_WIKI == 1
	       " -z | --list-supported-wiki         print supported devices in wiki syntax\n"
#endif
	       " -p | --programmer <name>[:<param>] specify the programmer device. One of\n");
	list_programmers_linebreak(4, 80, 0);
	printf(".\n\nYou can specify one of -h, -R, -L, "
#if CONFIG_PRINT_WIKI == 1
	         "-z, "
#endif
	         "-E, -r, -w, -v or no operation.\n"
	       "If no operation is specified, flashprog will only probe for flash chips.\n");
}

static void cli_classic_abort_usage(const char *msg)
{
	if (msg)
		fprintf(stderr, "%s", msg);
	printf("Please run \"flashprog --help\" for usage info.\n");
	exit(1);
}

static void cli_classic_validate_singleop(int *operation_specified)
{
	if (++(*operation_specified) > 1) {
		cli_classic_abort_usage("More than one operation specified. Aborting.\n");
	}
}

static int check_filename(char *filename, const char *type)
{
	if (!filename || (filename[0] == '\0')) {
		fprintf(stderr, "Error: No %s file specified.\n", type);
		return 1;
	}
	/* Not an error, but maybe the user intended to specify a CLI option instead of a file name. */
	if (filename[0] == '-' && filename[1] != '\0')
		fprintf(stderr, "Warning: Supplied %s file name starts with -\n", type);
	return 0;
}

/* Ensure a file is open by means of fstat */
static bool check_file(FILE *file)
{
	struct stat statbuf;

	if (fstat(fileno(file), &statbuf) < 0)
		return false;
	return true;
}

static int do_read(struct flashctx *const flash, const char *const filename)
{
	int ret;

	unsigned long size = flashprog_flash_getsize(flash);
	unsigned char *buf = calloc(size, sizeof(unsigned char));
	if (!buf) {
		msg_gerr("Memory allocation failed!\n");
		return 1;
	}

	ret = flashprog_image_read(flash, buf, size);
	if (ret > 0)
		goto free_out;

	ret = write_buf_to_file(buf, size, filename);

free_out:
	free(buf);
	return ret;
}

static int do_write(struct flashctx *const flash, const char *const filename, const char *const referencefile)
{
	const size_t flash_size = flashprog_flash_getsize(flash);
	int ret = 1;

	uint8_t *const newcontents = malloc(flash_size);
	uint8_t *const refcontents = referencefile ? malloc(flash_size) : NULL;

	if (!newcontents || (referencefile && !refcontents)) {
		msg_gerr("Out of memory!\n");
		goto _free_ret;
	}

	if (read_buf_from_file(newcontents, flash_size, filename))
		goto _free_ret;

	if (referencefile) {
		if (read_buf_from_file(refcontents, flash_size, referencefile))
			goto _free_ret;
	}

	ret = flashprog_image_write(flash, newcontents, flash_size, refcontents);

_free_ret:
	free(refcontents);
	free(newcontents);
	return ret;
}

static int do_verify(struct flashctx *const flash, const char *const filename)
{
	const size_t flash_size = flashprog_flash_getsize(flash);
	int ret = 1;

	uint8_t *const newcontents = malloc(flash_size);
	if (!newcontents) {
		msg_gerr("Out of memory!\n");
		goto _free_ret;
	}

	if (read_buf_from_file(newcontents, flash_size, filename))
		goto _free_ret;

	ret = flashprog_image_verify(flash, newcontents, flash_size);

_free_ret:
	free(newcontents);
	return ret;
}

/* Returns the number of buses commonly supported by the current programmer and flash chip where the latter
 * can not be completely accessed due to size/address limits of the programmer. */
static unsigned int count_max_decode_exceedings(const struct flashctx *flash,
		const struct decode_sizes *max_rom_decode_)
{
	unsigned int limitexceeded = 0;
	uint32_t size = flash->chip->total_size * 1024;
	enum chipbustype buses = flash->mst->buses_supported & flash->chip->bustype;

	if ((buses & BUS_PARALLEL) && (max_rom_decode_->parallel < size)) {
		limitexceeded++;
		msg_pdbg("Chip size %u kB is bigger than supported "
			 "size %u kB of chipset/board/programmer "
			 "for %s interface, "
			 "probe/read/erase/write may fail. ", size / 1024,
			 max_rom_decode_->parallel / 1024, "Parallel");
	}
	if ((buses & BUS_LPC) && (max_rom_decode_->lpc < size)) {
		limitexceeded++;
		msg_pdbg("Chip size %u kB is bigger than supported "
			 "size %u kB of chipset/board/programmer "
			 "for %s interface, "
			 "probe/read/erase/write may fail. ", size / 1024,
			 max_rom_decode_->lpc / 1024, "LPC");
	}
	if ((buses & BUS_FWH) && (max_rom_decode_->fwh < size)) {
		limitexceeded++;
		msg_pdbg("Chip size %u kB is bigger than supported "
			 "size %u kB of chipset/board/programmer "
			 "for %s interface, "
			 "probe/read/erase/write may fail. ", size / 1024,
			 max_rom_decode_->fwh / 1024, "FWH");
	}
	if ((buses & BUS_SPI) && (max_rom_decode_->spi < size)) {
		limitexceeded++;
		msg_pdbg("Chip size %u kB is bigger than supported "
			 "size %u kB of chipset/board/programmer "
			 "for %s interface, "
			 "probe/read/erase/write may fail. ", size / 1024,
			 max_rom_decode_->spi / 1024, "SPI");
	}
	return limitexceeded;
}

int main(int argc, char *argv[])
{
	const struct flashchip *chip = NULL;
	/* Probe for up to eight flash chips. */
	struct flashctx flashes[8] = {{0}};
	struct flashctx *fill_flash;
	const char *name;
	int namelen, opt, i, j;
	int startchip = -1, chipcount = 0, option_index = 0;
	int operation_specified = 0;
	bool force = false, ifd = false, fmap = false;
#if CONFIG_PRINT_WIKI == 1
	bool list_supported_wiki = false;
#endif
	bool flash_name = false, flash_size = false;
	bool read_it = false, write_it = false, erase_it = false, verify_it = false;
	bool dont_verify_it = false, dont_verify_all = false;
	bool list_supported = false;
	struct flashprog_layout *layout = NULL;
	static const struct programmer_entry *prog = NULL;
	enum {
		OPTION_IFD = 0x0100,
		OPTION_FMAP,
		OPTION_FMAP_FILE,
		OPTION_FLASH_CONTENTS,
		OPTION_FLASH_NAME,
		OPTION_FLASH_SIZE,
	};
	int ret = 0;

	static const char optstring[] = "r:Rw:v:nNVEfc:l:i:p:Lzho:";
	static const struct option long_options[] = {
		{"read",		1, NULL, 'r'},
		{"write",		1, NULL, 'w'},
		{"erase",		0, NULL, 'E'},
		{"verify",		1, NULL, 'v'},
		{"noverify",		0, NULL, 'n'},
		{"noverify-all",	0, NULL, 'N'},
		{"chip",		1, NULL, 'c'},
		{"verbose",		0, NULL, 'V'},
		{"force",		0, NULL, 'f'},
		{"layout",		1, NULL, 'l'},
		{"ifd",			0, NULL, OPTION_IFD},
		{"fmap",		0, NULL, OPTION_FMAP},
		{"fmap-file",		1, NULL, OPTION_FMAP_FILE},
		{"image",		1, NULL, 'i'}, // (deprecated): back compatibility.
		{"include",		1, NULL, 'i'},
		{"flash-contents",	1, NULL, OPTION_FLASH_CONTENTS},
		{"flash-name",		0, NULL, OPTION_FLASH_NAME},
		{"flash-size",		0, NULL, OPTION_FLASH_SIZE},
		{"get-size",		0, NULL, OPTION_FLASH_SIZE}, // (deprecated): back compatibility.
		{"list-supported",	0, NULL, 'L'},
		{"list-supported-wiki",	0, NULL, 'z'},
		{"programmer",		1, NULL, 'p'},
		{"help",		0, NULL, 'h'},
		{"version",		0, NULL, 'R'},
		{"output",		1, NULL, 'o'},
		{NULL,			0, NULL, 0},
	};

	char *filename = NULL;
	char *referencefile = NULL;
	char *layoutfile = NULL;
	char *fmapfile = NULL;
	char *logfile = NULL;
	char *tempstr = NULL;
	char *pparam = NULL;
	struct layout_include_args *include_args = NULL;

	/*
	 * Safety-guard against a user who has (mistakenly) closed
	 * stdout or stderr before exec'ing flashprog.  We disable
	 * logging in this case to prevent writing log data to a flash
	 * chip when a flash device gets opened with fd 1 or 2.
	 */
	if (check_file(stdout) && check_file(stderr)) {
		flashprog_set_log_callback(
			(flashprog_log_callback *)&flashprog_print_cb);
	}

	print_version();
	print_banner();

	/* FIXME: Delay calibration should happen in programmer code. */
	if (flashprog_init(1))
		exit(1);

	setbuf(stdout, NULL);
	/* FIXME: Delay all operation_specified checks until after command
	 * line parsing to allow --help overriding everything else.
	 */
	while ((opt = getopt_long(argc, argv, optstring,
				  long_options, &option_index)) != EOF) {
		switch (opt) {
		case 'r':
			cli_classic_validate_singleop(&operation_specified);
			filename = strdup(optarg);
			read_it = true;
			break;
		case 'w':
			cli_classic_validate_singleop(&operation_specified);
			filename = strdup(optarg);
			write_it = true;
			break;
		case 'v':
			//FIXME: gracefully handle superfluous -v
			cli_classic_validate_singleop(&operation_specified);
			if (dont_verify_it) {
				cli_classic_abort_usage("--verify and --noverify are mutually exclusive. Aborting.\n");
			}
			filename = strdup(optarg);
			verify_it = true;
			break;
		case 'n':
			if (verify_it) {
				cli_classic_abort_usage("--verify and --noverify are mutually exclusive. Aborting.\n");
			}
			dont_verify_it = true;
			break;
		case 'N':
			dont_verify_all = true;
			break;
		case 'c':
			chip_to_probe = strdup(optarg);
			break;
		case 'V':
			verbose_screen++;
			if (verbose_screen > FLASHPROG_MSG_DEBUG2)
				verbose_logfile = verbose_screen;
			break;
		case 'E':
			cli_classic_validate_singleop(&operation_specified);
			erase_it = true;
			break;
		case 'f':
			force = true;
			break;
		case 'l':
			if (layoutfile)
				cli_classic_abort_usage("Error: --layout specified more than once. Aborting.\n");
			if (ifd)
				cli_classic_abort_usage("Error: --layout and --ifd both specified. Aborting.\n");
			if (fmap)
				cli_classic_abort_usage("Error: --layout and --fmap-file both specified. Aborting.\n");
			layoutfile = strdup(optarg);
			break;
		case OPTION_IFD:
			if (layoutfile)
				cli_classic_abort_usage("Error: --layout and --ifd both specified. Aborting.\n");
			if (fmap)
				cli_classic_abort_usage("Error: --fmap-file and --ifd both specified. Aborting.\n");
			ifd = true;
			break;
		case OPTION_FMAP_FILE:
			if (fmap)
				cli_classic_abort_usage("Error: --fmap or --fmap-file specified "
					"more than once. Aborting.\n");
			if (ifd)
				cli_classic_abort_usage("Error: --fmap-file and --ifd both specified. Aborting.\n");
			if (layoutfile)
				cli_classic_abort_usage("Error: --fmap-file and --layout both specified. Aborting.\n");
			fmapfile = strdup(optarg);
			fmap = true;
			break;
		case OPTION_FMAP:
			if (fmap)
				cli_classic_abort_usage("Error: --fmap or --fmap-file specified "
					"more than once. Aborting.\n");
			if (ifd)
				cli_classic_abort_usage("Error: --fmap and --ifd both specified. Aborting.\n");
			if (layoutfile)
				cli_classic_abort_usage("Error: --layout and --fmap both specified. Aborting.\n");
			fmap = true;
			break;
		case 'i':
			tempstr = strdup(optarg);
			if (register_include_arg(&include_args, tempstr)) {
				free(tempstr);
				cli_classic_abort_usage(NULL);
			}
			break;
		case OPTION_FLASH_CONTENTS:
			if (referencefile)
				cli_classic_abort_usage("Error: --flash-contents specified more than once."
							"Aborting.\n");
			referencefile = strdup(optarg);
			break;
		case OPTION_FLASH_NAME:
			cli_classic_validate_singleop(&operation_specified);
			flash_name = true;
			break;
		case OPTION_FLASH_SIZE:
			cli_classic_validate_singleop(&operation_specified);
			flash_size = true;
			break;
		case 'L':
			cli_classic_validate_singleop(&operation_specified);
			list_supported = true;
			break;
		case 'z':
#if CONFIG_PRINT_WIKI == 1
			cli_classic_validate_singleop(&operation_specified);
			list_supported_wiki = true;
#else
			cli_classic_abort_usage("Error: Wiki output was not "
					"compiled in. Aborting.\n");
#endif
			break;
		case 'p':
			if (prog != NULL) {
				cli_classic_abort_usage("Error: --programmer specified "
					"more than once. You can separate "
					"multiple\nparameters for a programmer "
					"with \",\". Please see the man page "
					"for details.\n");
			}
			size_t p;
			for (p = 0; p < programmer_table_size; p++) {
				name = programmer_table[p]->name;
				namelen = strlen(name);
				if (strncmp(optarg, name, namelen) == 0) {
					switch (optarg[namelen]) {
					case ':':
						pparam = strdup(optarg + namelen + 1);
						if (!strlen(pparam)) {
							free(pparam);
							pparam = NULL;
						}
						prog = programmer_table[p];
						break;
					case '\0':
						prog = programmer_table[p];
						break;
					default:
						/* The continue refers to the
						 * for loop. It is here to be
						 * able to differentiate between
						 * foo and foobar.
						 */
						continue;
					}
					break;
				}
			}
			if (prog == NULL) {
				fprintf(stderr, "Error: Unknown programmer \"%s\". Valid choices are:\n",
					optarg);
				list_programmers_linebreak(0, 80, 0);
				msg_ginfo(".\n");
				cli_classic_abort_usage(NULL);
			}
			break;
		case 'R':
			/* print_version() is always called during startup. */
			cli_classic_validate_singleop(&operation_specified);
			exit(0);
			break;
		case 'h':
			cli_classic_validate_singleop(&operation_specified);
			cli_classic_usage(argv[0]);
			exit(0);
			break;
		case 'o':
			if (logfile) {
				fprintf(stderr, "Warning: -o/--output specified multiple times.\n");
				free(logfile);
			}

			logfile = strdup(optarg);
			if (logfile[0] == '\0') {
				cli_classic_abort_usage("No log filename specified.\n");
			}
			break;
		default:
			cli_classic_abort_usage(NULL);
			break;
		}
	}

	if (optind < argc)
		cli_classic_abort_usage("Error: Extra parameter found.\n");
	if ((read_it | write_it | verify_it) && check_filename(filename, "image"))
		cli_classic_abort_usage(NULL);
	if (layoutfile && check_filename(layoutfile, "layout"))
		cli_classic_abort_usage(NULL);
	if (fmapfile && check_filename(fmapfile, "fmap"))
		cli_classic_abort_usage(NULL);
	if (referencefile && check_filename(referencefile, "reference"))
		cli_classic_abort_usage(NULL);
	if (logfile && check_filename(logfile, "log"))
		cli_classic_abort_usage(NULL);
	if (logfile && open_logfile(logfile))
		cli_classic_abort_usage(NULL);

#if CONFIG_PRINT_WIKI == 1
	if (list_supported_wiki) {
		print_supported_wiki();
		goto out;
	}
#endif

	if (list_supported) {
		if (print_supported())
			ret = 1;
		goto out;
	}

	start_logging();

	print_buildinfo();
	msg_gdbg("Command line (%i args):", argc - 1);
	for (i = 0; i < argc; i++) {
		msg_gdbg(" %s", argv[i]);
	}
	msg_gdbg("\n");

	if (layoutfile && layout_from_file(&layout, layoutfile)) {
		ret = 1;
		goto out;
	}

	if (!ifd && !fmap && process_include_args(layout, include_args)) {
		ret = 1;
		goto out;
	}
	/* Does a chip with the requested name exist in the flashchips array? */
	if (chip_to_probe) {
		for (chip = flashchips; chip && chip->name; chip++)
			if (!strcmp(chip->name, chip_to_probe))
				break;
		if (!chip || !chip->name) {
			msg_cerr("Error: Unknown chip '%s' specified.\n", chip_to_probe);
			msg_gerr("Run flashprog -L to view the hardware supported in this flashprog version.\n");
			ret = 1;
			goto out;
		}
		/* Keep chip around for later usage in case a forced read is requested. */
	}

	if (prog == NULL) {
		const struct programmer_entry *const default_programmer = CONFIG_DEFAULT_PROGRAMMER_NAME;

		if (default_programmer) {
			prog = default_programmer;
			/* We need to strdup here because we free(pparam) unconditionally later. */
			pparam = strdup(CONFIG_DEFAULT_PROGRAMMER_ARGS);
			msg_pinfo("Using default programmer \"%s\" with arguments \"%s\".\n",
				  default_programmer->name, pparam);
		} else {
			msg_perr("Please select a programmer with the --programmer parameter.\n"
#if CONFIG_INTERNAL == 1
				 "To choose the mainboard of this computer use 'internal'. "
#endif
				 "Valid choices are:\n");
			list_programmers_linebreak(0, 80, 0);
			msg_ginfo(".\n");
			ret = 1;
			goto out;
		}
	}

	struct flashprog_programmer *flashprog;
	if (flashprog_programmer_init(&flashprog, prog->name, pparam)) {
		msg_perr("Error: Programmer initialization failed.\n");
		ret = 1;
		goto out;
	}
	tempstr = flashbuses_to_text(get_buses_supported());
	msg_pdbg("The following protocols are supported: %s.\n", tempstr);
	free(tempstr);

	for (j = 0; j < registered_master_count; j++) {
		startchip = 0;
		while (chipcount < (int)ARRAY_SIZE(flashes)) {
			startchip = probe_flash(&registered_masters[j], startchip, &flashes[chipcount], 0);
			if (startchip == -1)
				break;
			chipcount++;
			startchip++;
		}
	}

	if (chipcount > 1) {
		msg_cinfo("Multiple flash chip definitions match the detected chip(s): \"%s\"",
			  flashes[0].chip->name);
		for (i = 1; i < chipcount; i++)
			msg_cinfo(", \"%s\"", flashes[i].chip->name);
		msg_cinfo("\nPlease specify which chip definition to use with the -c <chipname> option.\n");
		ret = 1;
		goto out_shutdown;
	} else if (!chipcount) {
		msg_cinfo("No EEPROM/flash device found.\n");
		if (!force || !chip_to_probe) {
			msg_cinfo("Note: flashprog can never write if the flash chip isn't found "
				  "automatically.\n");
		}
		if (force && read_it && chip_to_probe) {
			struct registered_master *mst;
			int compatible_masters = 0;
			msg_cinfo("Force read (-f -r -c) requested, pretending the chip is there:\n");
			/* This loop just counts compatible controllers. */
			for (j = 0; j < registered_master_count; j++) {
				mst = &registered_masters[j];
				/* chip is still set from the chip_to_probe earlier in this function. */
				if (mst->buses_supported & chip->bustype)
					compatible_masters++;
			}
			if (!compatible_masters) {
				msg_cinfo("No compatible controller found for the requested flash chip.\n");
				ret = 1;
				goto out_shutdown;
			}
			if (compatible_masters > 1)
				msg_cinfo("More than one compatible controller found for the requested flash "
					  "chip, using the first one.\n");
			for (j = 0; j < registered_master_count; j++) {
				mst = &registered_masters[j];
				startchip = probe_flash(mst, 0, &flashes[0], 1);
				if (startchip != -1)
					break;
			}
			if (startchip == -1) {
				// FIXME: This should never happen! Ask for a bug report?
				msg_cinfo("Probing for flash chip '%s' failed.\n", chip_to_probe);
				ret = 1;
				goto out_shutdown;
			}
			msg_cinfo("Please note that forced reads most likely contain garbage.\n");
			flashprog_flag_set(&flashes[0], FLASHPROG_FLAG_FORCE, force);
			ret = do_read(&flashes[0], filename);
			free(flashes[0].chip);
			goto out_shutdown;
		}
		ret = 1;
		goto out_shutdown;
	} else if (!chip_to_probe) {
		/* repeat for convenience when looking at foreign logs */
		tempstr = flashbuses_to_text(flashes[0].chip->bustype);
		msg_gdbg("Found %s flash chip \"%s\" (%d kB, %s).\n",
			 flashes[0].chip->vendor, flashes[0].chip->name, flashes[0].chip->total_size, tempstr);
		free(tempstr);
	}

	fill_flash = &flashes[0];

	print_chip_support_status(fill_flash->chip);

	unsigned int limitexceeded = count_max_decode_exceedings(fill_flash, &max_rom_decode);
	if (limitexceeded > 0 && !force) {
		enum chipbustype commonbuses = fill_flash->mst->buses_supported & fill_flash->chip->bustype;

		/* Sometimes chip and programmer have more than one bus in common,
		 * and the limit is not exceeded on all buses. Tell the user. */
		if ((bitcount(commonbuses) > limitexceeded)) {
			msg_pdbg("There is at least one interface available which could support the size of\n"
				 "the selected flash chip.\n");
		}
		msg_cerr("This flash chip is too big for this programmer (--verbose/-V gives details).\n"
			 "Use --force/-f to override at your own risk.\n");
		ret = 1;
		goto out_shutdown;
	}

	if (!(read_it | write_it | verify_it | erase_it | flash_name | flash_size)) {
		msg_ginfo("No operations were specified.\n");
		goto out_shutdown;
	}

	if (flash_name) {
		if (fill_flash->chip->vendor && fill_flash->chip->name) {
			printf("vendor=\"%s\" name=\"%s\"\n",
				fill_flash->chip->vendor,
				fill_flash->chip->name);
		} else {
			ret = -1;
		}
		goto out_shutdown;
	}

	if (flash_size) {
		printf("%zu\n", flashprog_flash_getsize(fill_flash));
		goto out_shutdown;
	}

	if (ifd && (flashprog_layout_read_from_ifd(&layout, fill_flash, NULL, 0) ||
			   process_include_args(layout, include_args))) {
		ret = 1;
		goto out_shutdown;
	} else if (fmap && fmapfile) {
		struct stat s;
		if (stat(fmapfile, &s) != 0) {
			msg_gerr("Failed to stat fmapfile \"%s\"\n", fmapfile);
			ret = 1;
			goto out_shutdown;
		}

		size_t fmapfile_size = s.st_size;
		uint8_t *fmapfile_buffer = malloc(fmapfile_size);
		if (!fmapfile_buffer) {
			ret = 1;
			goto out_shutdown;
		}

		if (read_buf_from_file(fmapfile_buffer, fmapfile_size, fmapfile)) {
			ret = 1;
			free(fmapfile_buffer);
			goto out_shutdown;
		}

		if (flashprog_layout_read_fmap_from_buffer(&layout, fill_flash, fmapfile_buffer, fmapfile_size) ||
		    process_include_args(layout, include_args)) {
			ret = 1;
			free(fmapfile_buffer);
			goto out_shutdown;
		}
		free(fmapfile_buffer);
	} else if (fmap && (flashprog_layout_read_fmap_from_rom(&layout, fill_flash, 0,
				flashprog_flash_getsize(fill_flash)) || process_include_args(layout, include_args))) {
		ret = 1;
		goto out_shutdown;
	}

	flashprog_layout_set(fill_flash, layout);
	flashprog_flag_set(fill_flash, FLASHPROG_FLAG_FORCE, force);
#if CONFIG_INTERNAL == 1
	flashprog_flag_set(fill_flash, FLASHPROG_FLAG_FORCE_BOARDMISMATCH, force_boardmismatch);
#endif
	flashprog_flag_set(fill_flash, FLASHPROG_FLAG_VERIFY_AFTER_WRITE, !dont_verify_it);
	flashprog_flag_set(fill_flash, FLASHPROG_FLAG_VERIFY_WHOLE_CHIP, !dont_verify_all);

	/* FIXME: We should issue an unconditional chip reset here. This can be
	 * done once we have a .reset function in struct flashchip.
	 * Give the chip time to settle.
	 */
	programmer_delay(100000);
	if (read_it)
		ret = do_read(fill_flash, filename);
	else if (erase_it) {
		ret = flashprog_flash_erase(fill_flash);
		/*
		 * FIXME: Do we really want the scary warning if erase failed?
		 * After all, after erase the chip is either blank or partially
		 * blank or it has the old contents. A blank chip won't boot,
		 * so if the user wanted erase and reboots afterwards, the user
		 * knows very well that booting won't work.
		 */
		if (ret)
			emergency_help_message();
	}
	else if (write_it)
		ret = do_write(fill_flash, filename, referencefile);
	else if (verify_it)
		ret = do_verify(fill_flash, filename);

	flashprog_layout_release(layout);

out_shutdown:
	flashprog_programmer_shutdown(flashprog);
out:
	for (i = 0; i < chipcount; i++) {
		flashprog_layout_release(flashes[i].default_layout);
		free(flashes[i].chip);
	}

	cleanup_include_args(&include_args);
	free(filename);
	free(fmapfile);
	free(referencefile);
	free(layoutfile);
	free(pparam);
	/* clean up global variables */
	free((char *)chip_to_probe); /* Silence! Freeing is not modifying contents. */
	chip_to_probe = NULL;
	free(logfile);
	ret |= close_logfile();
	return ret;
}
