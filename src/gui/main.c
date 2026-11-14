/** dsynco: GUI exe
2026, Simon Zolin */

#include <dsynco.h>
#include <util/log.h>
#include <ffsys/path.h>
#include <ffsys/process.h>
#include <ffbase/args.h>

extern struct ds_coreinit ds_coreinit;

struct gsync;
extern struct gsync* gsync_create(const ds_core *core, const char *dir_src, const char *dir_dst);
extern void gsync_close(struct gsync *s);
extern void gsync_run(struct gsync *g);

struct exe {
	ds_core *core;
	char binfn[4096];
	ffstr rootdir;
	char *cmd_line;
	const char *input[2];
	ffvec args; // const char*[]

	struct zzlog	log;
	fftime			time_last;
	char			log_date[32];
};
static struct exe *x;

static int appdir_get(struct exe *x, char **argv)
{
	if (!ffps_filename(x->binfn, sizeof(x->binfn), argv[0]))
		return 1;
	ffpath_splitpath(x->binfn, ffsz_len(x->binfn), &x->rootdir, NULL);
	x->rootdir.ptr[x->rootdir.len] = '\0';
	return 0;
}

static void exe_logv(uint flags, const char *fmt, va_list args)
{
	if (x->core) {
		fftime tm;
		fftime_now(&tm);
		tm.sec += x->core->tz.real_offset;
		if (fftime_cmp(&tm, &x->time_last)) {
			x->time_last = tm;
			ffdatetime dt;
			fftime_split1(&dt, &tm);
			fftime_tostr1(&dt, x->log_date, sizeof(x->log_date), FFTIME_HMS_MSEC);
		}
	}

	uint64 tid = 0;
	zzlog_printv(&x->log, flags, NULL, tid, NULL, NULL, fmt, args);
}

void exe_log(uint flags, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	exe_logv(flags, fmt, args);
	va_end(args);
}

static void log_init(struct exe *x)
{
	if (!x->log.fd) {
		static const char levels[][8] = {
			"ERROR",
			"WARN ",
			"INFO ",
			"VERB ",
			"DEBUG",
		};
		ffmem_copy(x->log.levels, levels, sizeof(levels));

		static const char colors[][8] = {
			/*DS_LOG_ERR*/		FFSTD_CLR(FFSTD_RED),
			/*DS_LOG_WARN*/		FFSTD_CLR(FFSTD_YELLOW),
			/*DS_LOG_INFO*/		"",
			/*DS_LOG_VERBOSE*/	"",
			/*DS_LOG_DEBUG*/	"",
		};
		ffmem_copy(x->log.colors, colors, sizeof(colors));
	}

	x->log.fd = (!x->core) ? ffstdout : ffstderr;
	int r = ffstd_attr(x->log.fd, FFSTD_VTERM, FFSTD_VTERM);
	x->log.use_color = !r;
	x->log.fd_file = (r < 0);
}

static int core_load(struct exe *x)
{
	ds_coreinit.init();
	struct ds_core_conf cconf = {
		.timer_resolution_msec = 100,
		.log = exe_logv,
		.app_path = x->rootdir.ptr,
	};
	if (!(x->core = ds_coreinit.conf(&cconf)))
		return 1;
	return 0;
}

#define ARG_FIN  1
#define ARG_ERR  (-1)

static int gsync_help()
{
	ffstr s = FFSTR_Z("\
Show GUI for synchronizing directories\n\
Usage:\n\
    dsynco-gui [SOURCE TARGET]\n\
");
	ffstdout_write(s.ptr, s.len);
	return ARG_FIN;
}

static int arg_input(struct exe *x, const char *s)
{
	if (x->input[0] && x->input[1])
		return ARG_ERR;
	uint i = (x->input[0]) ? 1 : 0;
	x->input[i] = s;
	return 0;
}

static int args_parse(struct exe *x, uint argc, char **argv)
{
#ifdef FF_WIN
	// Prepare argv[] array
	x->cmd_line = ffsz_alloc_wtou(GetCommandLineW());
	ffstr line = FFSTR_Z(x->cmd_line), arg;
	while (!_ffargs_next(&line, &arg)) {
		arg.ptr[arg.len] = '\0';
		*ffvec_pushT(&x->args, char*) = arg.ptr;
	}
	ffvec_zpushT(&x->args, char*);
	argv = x->args.ptr;
	argc = x->args.len - 1;
#endif

	static const struct ffarg args[] = {
		{ "-help",	'1',	gsync_help },
		{ "\0\1",	's',	arg_input },
		{}
	};

	struct ffargs as = {};
	uint f = FFARGS_O_DUPLICATES | FFARGS_O_SKIP_FIRST;
	int r = ffargs_process_argv(&as, args, x, f, argv, argc);
	if (r > 0) {
		return 1;
	} else if (r < 0) {
		exe_log(DS_LOG_ERR, "command-line: %s", as.error);
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int ec = 1;

	x = ffmem_new(struct exe);
	struct gsync *g = NULL;

	log_init(x);
	if (appdir_get(x, argv)) goto exit;
	if (core_load(x)) goto exit;
	x->core->env = (const char**)environ;
	if (args_parse(x, argc, argv)) goto exit;

	g = gsync_create(x->core, x->input[0], x->input[1]);
	gsync_run(g);
	ec = ds_coreinit.run();

exit:
	gsync_close(g);
	ds_coreinit.destroy();
	ffvec_free(&x->args);
	ffmem_free(x);
	return ec;
}
