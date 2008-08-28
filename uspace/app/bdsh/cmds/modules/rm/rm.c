/* Copyright (c) 2008, Tim Post <tinkertim@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of the original program's authors nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <assert.h>
#include <getopt.h>

#include "config.h"
#include "errors.h"
#include "util.h"
#include "entry.h"
#include "rm.h"
#include "cmds.h"

static char *cmdname = "rm";
#define RM_VERSION "0.0.1"

static rm_job_t rm;

static struct option const long_options[] = {
	{ "help", no_argument, 0, 'h' },
	{ "version", no_argument, 0, 'v' },
	{ "recursive", no_argument, 0, 'r' },
	{ "force", no_argument, 0, 'f' },
	{ "safe", no_argument, 0, 's' },
	{ 0, 0, 0, 0 }
};

static unsigned int rm_start(rm_job_t *rm)
{
	rm->recursive = 0;
	rm->force = 0;
	rm->safe = 0;

	/* Make sure we can allocate enough memory to store
	 * what is needed in the job structure */
	if (NULL == (rm->nwd = (char *) malloc(PATH_MAX)))
		return 0;
	memset(rm->nwd, 0, sizeof(rm->nwd));

	if (NULL == (rm->owd = (char *) malloc(PATH_MAX)))
		return 0;
	memset(rm->owd, 0, sizeof(rm->owd));

	if (NULL == (rm->cwd = (char *) malloc(PATH_MAX)))
		return 0;
	memset(rm->cwd, 0, sizeof(rm->cwd));

	chdir(".");

	if (NULL == (getcwd(rm->owd, PATH_MAX)))
		return 0;

	return 1;
}

static void rm_end(rm_job_t *rm)
{
	if (NULL != rm->nwd)
		free(rm->nwd);

	if (NULL != rm->owd)
		free(rm->owd);

	if (NULL != rm->cwd)
		free(rm->cwd);

	return;
}

static unsigned int rm_recursive(const char *path)
{
	int rc;

	/* First see if it will just go away */
	rc = rmdir(path);
	if (rc == 0)
		return 0;

	/* Its not empty, recursively scan it */
	cli_error(CL_ENOTSUP,
		"Can not remove %s, directory not empty", path);
	return 1;
}

static unsigned int rm_single(const char *path)
{
	if (unlink(path)) {
		cli_error(CL_EFAIL, "rm: could not remove file %s", path);
		return 1;
	}
	return 0;
}

static unsigned int rm_scope(const char *path)
{
	int fd;
	DIR *dirp;

	dirp = opendir(path);
	if (dirp) {
		closedir(dirp);
		return RM_DIR;
	}

	fd = open(path, O_RDONLY);
	if (fd > 0) {
		close(fd);
		return RM_FILE;
	}

	return RM_BOGUS;
}

/* Dispays help for rm in various levels */
void * help_cmd_rm(unsigned int level)
{
	if (level == HELP_SHORT) {
		printf("`%s' removes files and directories.\n", cmdname);
	} else {
		help_cmd_rm(HELP_SHORT);
		printf(
		"Usage:  %s [options] <path>\n"
		"Options:\n"
		"  -h, --help       A short option summary\n"
		"  -v, --version    Print version information and exit\n"
		"  -r, --recursive  Recursively remove sub directories\n"
		"  -f, --force      Do not prompt prior to removing files\n"
		"  -s, --safe       Stop if directories change during removal\n\n"
		"Currently, %s is under development, some options don't work.\n",
		cmdname, cmdname);
	}
	return CMD_VOID;
}

/* Main entry point for rm, accepts an array of arguments */
int * cmd_rm(char **argv)
{
	unsigned int argc;
	unsigned int i, scope, ret = 0;
	int c, opt_ind;
	size_t len;
	char *buff = NULL;

	for (argc = 0; argv[argc] != NULL; argc ++);
	if (argc < 2) {
		cli_error(CL_EFAIL,
			"%s: insufficient arguments. Try %s --help", cmdname, cmdname);
		return CMD_FAILURE;
	}

	if (!rm_start(&rm)) {
		cli_error(CL_ENOMEM, "%s: could not initialize", cmdname);
		rm_end(&rm);
		return CMD_FAILURE;
	}

	for (c = 0, optind = 0, opt_ind = 0; c != -1;) {
		c = getopt_long(argc, argv, "hvrfs", long_options, &opt_ind);
		switch (c) {
		case 'h':
			help_cmd_rm(HELP_LONG);
			return CMD_SUCCESS;
		case 'v':
			printf("%s\n", RM_VERSION);
			return CMD_SUCCESS;
		case 'r':
			rm.recursive = 1;
			break;
		case 'f':
			rm.force = 1;
			break;
		case 's':
			rm.safe = 1;
			break;
		}
	}

	if (optind == argc) {
		cli_error(CL_EFAIL,
			"%s: insufficient arguments. Try %s --help", cmdname, cmdname);
		rm_end(&rm);
		return CMD_FAILURE;
	}

	i = optind;
	while (NULL != argv[i]) {
		len = strlen(argv[i]) + 2;
		buff = (char *) realloc(buff, len);
		assert(buff != NULL);
		memset(buff, 0, sizeof(buff));
		snprintf(buff, len, argv[i]);

		scope = rm_scope(buff);
		switch (scope) {
		case RM_BOGUS: /* FIXME */
		case RM_FILE:
			ret += rm_single(buff);
			break;
		case RM_DIR:
			if (! rm.recursive) {
				printf("%s is a directory, use -r to remove it.\n", buff);
				ret ++;
			} else {
				ret += rm_recursive(buff);
			}
			break;
		}
		i++;
	}

	if (NULL != buff)
		free(buff);

	rm_end(&rm);

	if (ret)
		return CMD_FAILURE;
	else
		return CMD_SUCCESS;
}

