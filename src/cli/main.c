/** dsynco: CLI
2022, Simon Zolin */

#include <dsynco.h>
#include <cli/args.h>
#include <util/log.h>
#include <ffsys/path.h>
#include <ffsys/process.h>
#include <ffsys/signal.h>
#include <ffsys/thread.h>
#include <ffsys/globals.h>

extern struct ds_coreinit ds_coreinit;

struct main {
	struct ds_coreinit *ci;
	struct ds_core *core;

	struct zzlog	log;
	fftime			time_last;
	char			log_date[32];

	struct args args;
	char binfn[4096];
	ffstr rootdir;
};
static struct main *x;

#include <cli/log.h>

#if defined FF_WIN
	#define OS_STR  "windows"
#else
	#define OS_STR  "linux"
#endif

#if defined FF_AMD64
	#define CPU_STR  "amd64"
#endif

static void version_print()
{
	char buf[128];
	uint n = ffs_format_r0(buf, sizeof(buf), "dsynco v%s (%s-%s)\n"
		, x->core->version_str, OS_STR, CPU_STR);
	ffstdout_write(buf, n);
}

static int appdir_get(struct main *x, char **argv)
{
	if (!ffps_filename(x->binfn, sizeof(x->binfn), argv[0]))
		return 1;
	ffpath_splitpath(x->binfn, ffsz_len(x->binfn), &x->rootdir, NULL);
	x->rootdir.ptr[x->rootdir.len] = '\0';
	return 0;
}

static int core_load()
{
	log_init(x);

	ds_coreinit.init();
	struct ds_core_conf cconf = {
		.timer_resolution_msec = 100,
		.log = exe_logv,
		.app_path = x->rootdir.ptr,
		.debug = x->args.debug,
		.verbose = x->args.verbose,
		.stdout_color = !ffstd_attr(ffstdout, FFSTD_VTERM, FFSTD_VTERM),
	};
	if (!(x->core = ds_coreinit.conf(&cconf)))
		return 1;
	return 0;
}

static void on_signal(struct ffsig_info *i)
{
	exe_log(DS_LOG_DBG, "received system signal: %u", i->sig);
	x->core->exit(1);
}

static void main_free()
{
	ds_coreinit.destroy();
	args_destroy(&x->args);
	ffmem_free(x);
}

static void done(void *param, int code)
{
	struct main *x = param;
	x->core->exit(code);
}

int main(int argc, char **argv)
{
	int ec = 1;

	x = ffmem_new(struct main);
	if (appdir_get(x, argv))
		goto exit;

	int r;
	if ((r = args_read(&x->args, argc, argv))
		&& !x->args.version) {
		if (r > 0)
			ec = 0;
		goto exit;
	}

	if (core_load())
		goto exit;
	x->core->env = (const char**)environ;

	if (x->args.version) {
		version_print();
		ec = 0;
		goto exit;
	}

	static const uint sigs[] = { FFSIG_INT };
	ffsig_subscribe(on_signal, sigs, FF_COUNT(sigs));

	x->args.sc.on_complete = done;
	x->args.sc.opaque = x;
	ds_sync_run(&x->args.sc);
	ec = ds_coreinit.run();

exit:
	main_free();
	return ec;
}
