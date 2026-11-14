/** dsynco: CLI arguments
2022, Simon Zolin */

#pragma once
#include <ffsys/dirscan.h>
#include <ffsys/std.h>
#include <ffbase/args.h>

enum MODE {
	MODE_DIFF,
	MODE_SYNC,
};

struct args_diff {
	u_char add, del, move, update;
	char *output;
};

struct args_sync {
	u_char add, del, move, update, test;
	char *output;
};

struct args {
	union {
	struct args_diff diff;
	struct args_sync sync;
	};

	u_char debug;
	u_char verbose;
	u_char version;

	uint mode; // enum MODE
	ffvec args; // const char*[]
	char *cmd_line;

	struct ds_sync_conf sc;
};

void exe_log(uint flags, const char *fmt, ...);

#define ARG_FIN  1
#define ARG_ERR  (-1)

static void help_info_write(ffstr s)
{
	ffstr l, k;
	ffvec v = {};

	int use_color = !ffstd_attr(ffstdout, FFSTD_VTERM, FFSTD_VTERM);

	const char *clr = FFSTD_CLR_B(FFSTD_PURPLE);
	while (s.len) {
		ffstr_splitby(&s, '`', &l, &s);
		ffstr_splitby(&s, '`', &k, &s);
		if (use_color) {
			ffvec_addfmt(&v, "%S%s%S%s"
				, &l, clr, &k, FFSTD_CLR_RESET);
		} else {
			ffvec_addfmt(&v, "%S%S"
				, &l, &k);
		}
	}

	ffstdout_write(v.ptr, v.len);
	ffvec_free(&v);
}

static int args_help(struct args *a)
{
	(void)a;
	help_info_write(FFSTR_Z("\
Usage:\n\
  dsynco [GLOBAL] COMMAND SOURCE -o TARGET [OPTIONS]\n\
\n\
GLOBAL:\n\
  `-verbose` Verbose output\n\
  `-debug` Debug output\n\
  `-help` Show help\n\
\n\
COMMAND:\n\
  `diff` Show difference table\n\
  `sync` Perform 1-way synchronization\n\
\n\
'dsynco COMMAND -help' will print information on a particular command.\n\
\n\
Examples:\n\
  dsynco diff /home/a -o /home/b\n\
  dsynco sync /home/a -o /home/b\n\
"));
	return ARG_FIN;
}

static int args_usage(struct args *a)
{
	(void)a;
	help_info_write(FFSTR_Z("\
Usage:\n\
  dsynco [GLOBAL] COMMAND SOURCE -o TARGET [OPTIONS]\n\
\n\
COMMAND must be one of: `diff`, `sync`\n\
Run `dsynco -help` for complete help info.\n\
"));
	return ARG_FIN;
}

static int args_version(struct args *a)
{
	a->version = 1;
	return ARG_FIN;
}

#ifdef FF_WIN
static int wc_expand(struct args *a, ffstr *s)
{
	int rc = ARG_ERR;
	ffstr dir, name;
	ffpath_splitpath(s->ptr, s->len, &dir, &name);
	if (dir.len == 0)
		ffstr_setz(&dir, ".");
	char *dirz = ffsz_dupstr(&dir);

	ffdirscan ds = {};
	ds.wildcard = ffsz_dupstr(&name);
	if (0 != ffdirscan_open(&ds, dirz, FFDIRSCAN_USEWILDCARD)) {
		exe_log(DS_LOG_SYSERR | DS_LOG_ERR, "dir open: %s", dirz);
		goto err;
	}

	const char *fn;
	while (NULL != (fn = ffdirscan_next(&ds))) {
		exe_log(DS_LOG_DBG, "wildcard expand: %s", fn);
		ffstr *p = ffvec_zpushT(&a->sc.input, ffstr);
		ffsize cap = 0;
		ffstr_growfmt(p, &cap, "%S\\%s", &dir, fn);
	}

	rc = 0;

err:
	ffmem_free(dirz);
	ffmem_free((char*)ds.wildcard);
	ffdirscan_close(&ds);
	return rc;
}
#endif

static int arg_input(struct args *a, ffstr s)
{
#ifdef FF_WIN
	if (ffstr_findany(&s, "*?", 2) >= 0
		&& !ffstr_matchz(&s, "\\\\?\\")) {
		return wc_expand(a, &s);
	}
#endif

	ffstr *p = ffvec_zpushT(&a->sc.input, ffstr);
	p->ptr = ffsz_dupstr(&s);
	p->len = s.len;
	return 0;
}

#include <cli/diff.h>
#include <cli/sync.h>

#define O(m)  (void*)FF_OFF(struct args, m)
static const struct ffarg cmd_root[] = {
	{ "-debug",    '1',  O(debug) },
	{ "-help",     '0',  args_help },
	{ "-v",        '1',  O(verbose) },
	{ "-verbose",  '1',  O(verbose) },
	{ "-version",  '0',  args_version },
	{ "diff",      '{',  cmd_diff_init },
	{ "sync",      '{',  cmd_sync_init },
	{ "\0\1",      's',  args_usage },
	{ "",          '0',  args_usage },
};
#undef O

int args_read(struct args *a, uint argc, char **argv)
{
#ifdef FF_WIN
	// Prepare argv[] array
	a->cmd_line = ffsz_alloc_wtou(GetCommandLineW());
	ffstr line = FFSTR_Z(a->cmd_line), arg;
	while (!_ffargs_next(&line, &arg)) {
		arg.ptr[arg.len] = '\0';
		*ffvec_pushT(&a->args, char*) = arg.ptr;
	}
	ffvec_zpushT(&a->args, char*);
	argv = a->args.ptr;
	argc = a->args.len - 1;
#endif

	struct ffargs as = {};
	uint f = FFARGS_O_PARTIAL | FFARGS_O_DUPLICATES | FFARGS_O_SKIP_FIRST;
	int r = ffargs_process_argv(&as, cmd_root, a, f, argv, argc);
	if (r > 0) {
		return 1;
	} else if (r < 0) {
		exe_log(DS_LOG_ERR, "command-line: %s", as.error);
		return -1;
	}
	return 0;
}

void args_destroy(struct args *a)
{
	ffvec_free(&a->args);
	ffmem_free(a->cmd_line);
}
