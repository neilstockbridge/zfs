/*
 * This file is part of the ZFS Linux port.
 *
 *  Copyright (c) 2008 Lawrence Livermore National Security, LLC.
 *  Produced at Lawrence Livermore National Laboratory
 *  Written by:
 *          Brian Behlendorf <behlendorf1@llnl.gov>,
 *          Herb Wartens <wartens2@llnl.gov>,
 *          Jim Garlick <garlick@llnl.gov>
 * LLNL-CODE-403049
 *
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 *
 * Kernel PIOS DMU implemenation originally derived from PIOS test code.
 * Character control interface derived from SPL code.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "zpios.h"

static const char short_opt[] = "t:l:h:e:n:i:j:k:c:u:a:b:g:L:P:R:I:"
                                "G:T:Vzs:A:B:C:o:m:q:r:fwxdp:v?";
static const struct option long_opt[] = {
	{"chunksize",           required_argument, 0, 'c' },
	{"chunksize_low",       required_argument, 0, 'a' },
	{"chunksize_high",      required_argument, 0, 'b' },
	{"chunksize_incr",      required_argument, 0, 'g' },
	{"offset",              required_argument, 0, 'o' },
	{"offset_low",          required_argument, 0, 'm' },
	{"offset_high",         required_argument, 0, 'q' },
	{"offset_incr",         required_argument, 0, 'r' },
	{"regioncount",         required_argument, 0, 'n' },
	{"regioncount_low",     required_argument, 0, 'i' },
	{"regioncount_high",    required_argument, 0, 'j' },
	{"regioncount_incr",    required_argument, 0, 'k' },
	{"threadcount",         required_argument, 0, 't' },
	{"threadcount_low",     required_argument, 0, 'l' },
	{"threadcount_high",    required_argument, 0, 'h' },
	{"threadcount_incr",    required_argument, 0, 'e' },
	{"regionsize",          required_argument, 0, 's' },
	{"regionsize_low",      required_argument, 0, 'A' },
	{"regionsize_high",     required_argument, 0, 'B' },
	{"regionsize_incr",     required_argument, 0, 'C' },
	{"cleanup",             no_argument,       0, 'x' },
	{"verify",              no_argument,       0, 'V' },
	{"zerocopy",            no_argument,       0, 'z' },
	{"threaddelay",         required_argument, 0, 'T' },
	{"regionnoise",         required_argument, 0, 'I' },
	{"chunknoise",          required_argument, 0, 'N' },
	{"prerun",              required_argument, 0, 'P' },
	{"postrun",             required_argument, 0, 'R' },
	{"log",                 required_argument, 0, 'G' },
	{"path",                required_argument, 0, 'p' },
	{"pool",                required_argument, 0, 'p' },
	{"load",                required_argument, 0, 'L' },
	{"human-readable",      no_argument,       0, 'H' },
	{"help",                no_argument,       0, '?' },
	{"verbose",             no_argument,       0, 'v' },
	{ 0,                    0,                 0,  0  },
};

static int zpiosctl_fd;				/* Control file descriptor */
static char zpios_version[VERSION_SIZE];	/* Kernel version string */
static char *zpios_buffer = NULL;		/* Scratch space area */
static int zpios_buffer_size = 0;		/* Scratch space size */

static int
usage(void)
{
	fprintf(stderr, "Usage: zpios\n");
	fprintf(stderr,
	        "	--chunksize         -c    =values\n"
	        "	--chunksize_low     -a    =value\n"
	        "	--chunksize_high    -b    =value\n"
	        "	--chunksize_incr    -g    =value\n"
	        "	--offset            -o    =values\n"
	        "	--offset_low        -m    =value\n"
	        "	--offset_high       -q    =value\n"
	        "	--offset_incr       -r    =value\n"
	        "	--regioncount       -n    =values\n"
	        "	--regioncount_low   -i    =value\n"
	        "	--regioncount_high  -j    =value\n"
	        "	--regioncount_incr  -k    =value\n"
	        "	--threadcount       -t    =values\n"
	        "	--threadcount_low   -l    =value\n"
	        "	--threadcount_high  -h    =value\n"
	        "	--threadcount_incr  -e    =value\n"
	        "	--regionsize        -s    =values\n"
	        "	--regionsize_low    -A    =value\n"
	        "	--regionsize_high   -B    =value\n"
	        "	--regionsize_incr   -C    =value\n"
	        "	--cleanup           -x\n"
	        "	--verify            -V\n"
	        "	--zerocopy          -z\n"
	        "	--threaddelay       -T    =jiffies\n"
	        "	--regionnoise       -I    =shift\n"
	        "	--chunknoise        -N    =bytes\n"
	        "	--prerun            -P    =pre-command\n"
	        "	--postrun           -R    =post-command\n"
		"       --log               -G    =log directory\n"
	        "	--pool | --path     -p    =pool name\n"
	        "	--load              -L    =dmuio\n"
		"       --human-readable    -H\n"
	        "	--help              -?    =this help\n"
	        "	--verbose           -v    =increase verbosity\n\n");

	return 0;
}

static void args_fini(cmd_args_t *args)
{
	assert(args != NULL);
	free(args);
}

static cmd_args_t *
args_init(int argc, char **argv)
{
	cmd_args_t *args;
	uint32_t fl_th = 0;
	uint32_t fl_rc = 0;
	uint32_t fl_of = 0;
	uint32_t fl_rs = 0;
	uint32_t fl_cs = 0;
	int c, rc;

	if (argc == 1) {
		usage();
		return (cmd_args_t *)NULL;
	}

	/* Configure and populate the args structures */
	args = malloc(sizeof(*args));
	if (args == NULL)
		return NULL;

	memset(args, 0, sizeof(*args));

	while ((c=getopt_long(argc, argv, short_opt, long_opt, NULL)) != -1) {
		rc = 0;

		switch (c) {
		case 'v': /* --verbose */
			args->verbose++;
			break;
		case 't': /* --thread count */
		        rc = set_count(REGEX_NUMBERS, REGEX_NUMBERS_COMMA,
				       &args->T, optarg, &fl_th, "threadcount");
			break;
		case 'l': /* --threadcount_low */
			rc = set_lhi(REGEX_NUMBERS, &args->T, optarg,
			             FLAG_LOW, &fl_th, "threadcount_low");
			break;
		case 'h': /* --threadcount_high */
			rc = set_lhi(REGEX_NUMBERS, &args->T, optarg,
			             FLAG_HIGH, &fl_th, "threadcount_high");
			break;
		case 'e': /* --threadcount_inc */
			rc = set_lhi(REGEX_NUMBERS, &args->T, optarg,
			             FLAG_INCR, &fl_th, "threadcount_incr");
			break;
		case 'n': /* --regioncount */
			rc = set_count(REGEX_NUMBERS, REGEX_NUMBERS_COMMA,
				       &args->N, optarg, &fl_rc, "regioncount");
			break;
		case 'i': /* --regioncount_low */
			rc = set_lhi(REGEX_NUMBERS, &args->N, optarg,
			             FLAG_LOW, &fl_rc, "regioncount_low");
			break;
		case 'j': /* --regioncount_high */
			rc = set_lhi(REGEX_NUMBERS, &args->N, optarg,
			             FLAG_HIGH, &fl_rc, "regioncount_high");
			break;
		case 'k': /* --regioncount_inc */
			rc = set_lhi(REGEX_NUMBERS, &args->N, optarg,
			             FLAG_INCR, &fl_rc, "regioncount_incr");
			break;
		case 'o': /* --offset */
			rc = set_count(REGEX_SIZE, REGEX_SIZE_COMMA,
				       &args->O, optarg, &fl_of, "offset");
			break;
		case 'm': /* --offset_low */
			rc = set_lhi(REGEX_SIZE, &args->O, optarg,
			             FLAG_LOW, &fl_of, "offset_low");
			break;
		case 'q': /* --offset_high */
			rc = set_lhi(REGEX_SIZE, &args->O, optarg,
			             FLAG_HIGH, &fl_of, "offset_high");
			break;
		case 'r': /* --offset_inc */
			rc = set_lhi(REGEX_NUMBERS, &args->O, optarg,
			             FLAG_INCR, &fl_of, "offset_incr");
			break;
		case 'c': /* --chunksize */
			rc = set_count(REGEX_SIZE, REGEX_SIZE_COMMA,
				       &args->C, optarg, &fl_cs, "chunksize");
			break;
		case 'a': /* --chunksize_low */
			rc = set_lhi(REGEX_SIZE, &args->C, optarg,
			             FLAG_LOW, &fl_cs, "chunksize_low");
			break;
		case 'b': /* --chunksize_high */
			rc = set_lhi(REGEX_SIZE, &args->C, optarg,
			             FLAG_HIGH, &fl_cs, "chunksize_high");
			break;
		case 'g': /* --chunksize_inc */
			rc = set_lhi(REGEX_NUMBERS, &args->C, optarg,
			             FLAG_INCR, &fl_cs, "chunksize_incr");
			break;
		case 's': /* --regionsize */
			rc = set_count(REGEX_SIZE, REGEX_SIZE_COMMA,
				       &args->S, optarg, &fl_rs, "regionsize");
			break;
		case 'A': /* --regionsize_low */
			rc = set_lhi(REGEX_SIZE, &args->S, optarg,
			             FLAG_LOW, &fl_rs, "regionsize_low");
			break;
		case 'B': /* --regionsize_high */
			rc = set_lhi(REGEX_SIZE, &args->S, optarg,
			             FLAG_HIGH, &fl_rs, "regionsize_high");
			break;
		case 'C': /* --regionsize_inc */
			rc = set_lhi(REGEX_NUMBERS, &args->S, optarg,
			             FLAG_INCR, &fl_rs, "regionsize_incr");
			break;
		case 'L': /* --load */
			rc = set_load_params(args, optarg);
			break;
		case 'p': /* --pool */
			args->pool = optarg;
			break;
		case 'x': /* --cleanup */
			args->flags |= DMU_REMOVE;
			break;
		case 'P': /* --prerun */
			strncpy(args->pre, optarg, ZPIOS_PATH_SIZE - 1);
			break;
		case 'R': /* --postrun */
			strncpy(args->post, optarg, ZPIOS_PATH_SIZE - 1);
			break;
		case 'G': /* --log */
			strncpy(args->log, optarg, ZPIOS_PATH_SIZE - 1);
			break;
		case 'I': /* --regionnoise */
			rc = set_noise(&args->regionnoise, optarg, "regionnoise");
			break;
		case 'N': /* --chunknoise */
			rc = set_noise(&args->chunknoise, optarg, "chunknoise");
			break;
		case 'T': /* --threaddelay */
			rc = set_noise(&args->thread_delay, optarg, "threaddelay");
			break;
		case 'V': /* --verify */
			args->flags |= DMU_VERIFY;
			break;
		case 'z': /* --verify */
			args->flags |= (DMU_WRITE_ZC | DMU_READ_ZC);
			break;
		case 'H':
			args->human_readable = 1;
			break;
		case '?':
			rc = 1;
			break;
		default:
			fprintf(stderr,"Unknown option '%s'\n",argv[optind-1]);
			rc = EINVAL;
			break;
		}

		if (rc) {
			usage();
			args_fini(args);
			return NULL;
		}
	}

	check_mutual_exclusive_command_lines(fl_th, "threadcount");
	check_mutual_exclusive_command_lines(fl_rc, "regioncount");
	check_mutual_exclusive_command_lines(fl_of, "offset");
	check_mutual_exclusive_command_lines(fl_rs, "regionsize");
	check_mutual_exclusive_command_lines(fl_cs, "chunksize");

	if (args->pool == NULL) {
		fprintf(stderr, "Error: Pool not specificed\n");
		usage();
		args_fini(args);
		return NULL;
	}

	if ((args->flags & (DMU_WRITE_ZC | DMU_READ_ZC)) &&
	    (args->flags & DMU_VERIFY)) {
                fprintf(stderr, "Error, --zerocopy incompatible --verify, "
                            "used for performance analysis only\n");
		usage();
		args_fini(args);
		return NULL;
	}

	return args;
}

static int
dev_clear(void)
{
	zpios_cfg_t cfg;
	int rc;

	memset(&cfg, 0, sizeof(cfg));
	cfg.cfg_magic = ZPIOS_CFG_MAGIC;
        cfg.cfg_cmd   = ZPIOS_CFG_BUFFER_CLEAR;
	cfg.cfg_arg1  = 0;

	rc = ioctl(zpiosctl_fd, ZPIOS_CFG, &cfg);
	if (rc)
		fprintf(stderr, "Ioctl() error %lu / %d: %d\n",
		        (unsigned long) ZPIOS_CFG, cfg.cfg_cmd, errno);

	lseek(zpiosctl_fd, 0, SEEK_SET);

	return rc;
}

/* Passing a size of zero simply results in querying the current size */
static int
dev_size(int size)
{
	zpios_cfg_t cfg;
	int rc;

	memset(&cfg, 0, sizeof(cfg));
	cfg.cfg_magic = ZPIOS_CFG_MAGIC;
        cfg.cfg_cmd   = ZPIOS_CFG_BUFFER_SIZE;
	cfg.cfg_arg1  = size;

	rc = ioctl(zpiosctl_fd, ZPIOS_CFG, &cfg);
	if (rc) {
		fprintf(stderr, "Ioctl() error %lu / %d: %d\n",
		        (unsigned long) ZPIOS_CFG, cfg.cfg_cmd, errno);
		return rc;
	}

	return cfg.cfg_rc1;
}

static void
dev_fini(void)
{
	if (zpios_buffer)
		free(zpios_buffer);

	if (zpiosctl_fd != -1) {
		if (close(zpiosctl_fd) == -1) {
			fprintf(stderr, "Unable to close %s: %d\n",
		                ZPIOS_DEV, errno);
		}
	}
}

static int
dev_init(void)
{
	int rc;

	zpiosctl_fd = open(ZPIOS_DEV, O_RDONLY);
	if (zpiosctl_fd == -1) {
		fprintf(stderr, "Unable to open %s: %d\n"
		        "Is the zpios module loaded?\n", ZPIOS_DEV, errno);
		rc = errno;
		goto error;
	}

	if ((rc = dev_clear()))
		goto error;

	if ((rc = dev_size(0)) < 0)
		goto error;

	zpios_buffer_size = rc;
	zpios_buffer = (char *)malloc(zpios_buffer_size);
	if (zpios_buffer == NULL) {
		rc = ENOMEM;
		goto error;
	}

	memset(zpios_buffer, 0, zpios_buffer_size);
	return 0;
error:
	if (zpiosctl_fd != -1) {
		if (close(zpiosctl_fd) == -1) {
			fprintf(stderr, "Unable to close %s: %d\n",
		                ZPIOS_DEV, errno);
		}
	}

	return rc;
}

static int
get_next(uint64_t *val, range_repeat_t *range)
{
	int i;

	/* if low, incr, high is given */
	if (range->val_count == 0) {
		*val = (range->val_low) +
		       (range->val_low * range->next_val / 100);

		if (*val > range->val_high)
			return 0; /* No more values, limit exceeded */

		if (!range->next_val)
			range->next_val = range->val_inc_perc;
		else
			range->next_val = range->next_val+range->val_inc_perc;

		return 1; /* more values to come */

	/* if only one val is given */
	} else if (range->val_count == 1) {
		if (range->next_val)
			return 0; /* No more values, we only have one */

		*val = range->val[0];
		range->next_val = 1;
		return 1; /* more values to come */

	/* if comma separated values are given */
	} else if (range->val_count > 1) {
		if (range->next_val > range->val_count - 1)
			return 0; /* No more values, limit exceeded */

		*val = range->val[range->next_val];
		range->next_val++;
		return 1; /* more values to come */
	}

	return 0;
}

static int
run_one(cmd_args_t *args, uint32_t id, uint32_t T, uint32_t N,
        uint64_t C, uint64_t S, uint64_t O)
{
	zpios_cmd_t *cmd;
        int rc, rc2, cmd_size;

        dev_clear();

	cmd_size = sizeof(zpios_cmd_t) + ((T + N + 1) * sizeof(zpios_stats_t));
        cmd = (zpios_cmd_t *)malloc(cmd_size);
        if (cmd == NULL)
                return ENOMEM;

        memset(cmd, 0, cmd_size);
        cmd->cmd_magic = ZPIOS_CMD_MAGIC;
	strncpy(cmd->cmd_pool, args->pool, ZPIOS_NAME_SIZE - 1);
	strncpy(cmd->cmd_pre, args->pre, ZPIOS_PATH_SIZE - 1);
	strncpy(cmd->cmd_post, args->post, ZPIOS_PATH_SIZE - 1);
	strncpy(cmd->cmd_log, args->log, ZPIOS_PATH_SIZE - 1);
	cmd->cmd_id           = id;
	cmd->cmd_chunk_size   = C;
	cmd->cmd_thread_count = T;
	cmd->cmd_region_count = N;
	cmd->cmd_region_size  = S;
	cmd->cmd_offset       = O;
	cmd->cmd_region_noise = args->regionnoise;
	cmd->cmd_chunk_noise  = args->chunknoise;
	cmd->cmd_thread_delay = args->thread_delay;
	cmd->cmd_flags        = args->flags;
        cmd->cmd_data_size    = (T + N + 1) * sizeof(zpios_stats_t);

        rc = ioctl(zpiosctl_fd, ZPIOS_CMD, cmd);
	if (rc)
		args->rc = errno;

	print_stats(args, cmd);

        if (args->verbose) {
                rc2 = read(zpiosctl_fd, zpios_buffer, zpios_buffer_size - 1);
                if (rc2 < 0) {
                        fprintf(stdout, "Error reading results: %d\n", rc2);
                } else if ((rc2 > 0) && (strlen(zpios_buffer) > 0)) {
                        fprintf(stdout, "\n%s\n", zpios_buffer);
                        fflush(stdout);
                }
        }

        free(cmd);

        return rc;
}

static int
run_offsets(cmd_args_t *args)
{
	int rc = 0;

	while (rc == 0 && get_next(&args->current_O, &args->O)) {
		rc = run_one(args, args->current_id,
		             args->current_T, args->current_N, args->current_C,
		             args->current_S, args->current_O);
		args->current_id++;
	}

	args->O.next_val = 0;
	return rc;
}

static int
run_region_counts(cmd_args_t *args)
{
	int rc = 0;

	while (rc == 0 && get_next((uint64_t *)&args->current_N, &args->N))
	       rc = run_offsets(args);

	args->N.next_val = 0;
	return rc;
}

static int
run_region_sizes(cmd_args_t *args)
{
	int rc = 0;

	while (rc == 0 && get_next(&args->current_S, &args->S)) {
		if (args->current_S < args->current_C) {
			fprintf(stderr, "Error: in any run chunksize can "
				"not be smaller than regionsize.\n");
			return EINVAL;
		}

		rc = run_region_counts(args);
	}

	args->S.next_val = 0;
	return rc;
}

static int
run_chunk_sizes(cmd_args_t *args)
{
	int rc = 0;

	while (rc == 0 && get_next(&args->current_C, &args->C)) {
	       rc = run_region_sizes(args);
	}

	args->C.next_val = 0;
	return rc;
}

static int
run_thread_counts(cmd_args_t *args)
{
	int rc = 0;

	while (rc == 0 && get_next((uint64_t *)&args->current_T, &args->T))
		rc = run_chunk_sizes(args);

	return rc;
}

int
main(int argc, char **argv)
{
	cmd_args_t *args;
	int rc = 0;

	/* Argument init and parsing */
	if ((args = args_init(argc, argv)) == NULL) {
		rc = -1;
		goto out;
	}

	/* Device specific init */
	if ((rc = dev_init()))
		goto out;

	/* Generic kernel version string */
	if (args->verbose)
		fprintf(stdout, "%s", zpios_version);

	print_stats_header();
	rc = run_thread_counts(args);
out:
	if (args != NULL)
		args_fini(args);

	dev_fini();
	return rc;
}