/** dsynco: core: async events delivery, ds_core implementation.
2022, Simon Zolin */

#include <dsynco.h>
#include <ffsys/queue.h>
#include <ffsys/timer.h>
#include <ffsys/path.h>
#include <ffsys/perf.h>
#include <ffsys/globals.h>

#ifndef DS_VER
	#define DS_VER  "test"
#endif

#define syserrlog(fmt, ...)  core_log(DS_LOG_ERR | DS_LOG_SYSERR, "core: " fmt, ##__VA_ARGS__)
#define errlog(fmt, ...)  core_log(DS_LOG_ERR, "core: " fmt, ##__VA_ARGS__)
#define dbglog(fmt, ...)  ds_dbglog("core: " fmt, ##__VA_ARGS__)

ds_core _ds_core;
ds_core *core = &_ds_core;
extern const struct ds_sync_if sync_if;

struct core {
	struct ds_core_conf conf;

	ffkq kq;
	uint kq_quit;
	uint exit_code;
	ds_log_func logger;
	ffkq_postevent kq_wake;
	ds_kevent kq_wake_ev;
	ds_task kq_wake_task;
	fftaskqueue taskq;

	fftimer tmr;
	ds_kevent tmr_kev;
	fftimerqueue tq;
	uint now_msec;
	fftime now, utc;

	uint rnd_seed;
};
static struct core *gcore;

static void core_logv(uint flags, const char *fmt, va_list args)
{
	gcore->conf.log(flags, fmt, args);
}

static void core_log(uint flags, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	gcore->conf.log(flags, fmt, args);
	va_end(args);
}

static char* core_path(const char *name)
{
	char *s;
	if (ffpath_abs(name, ffsz_len(name)))
		s = ffsz_dup(name);
	else
		s = ffsz_allocfmt("%s%c%s", gcore->conf.app_path, FFPATH_SLASH, name);
	dbglog("path: '%s' -> '%s'", name, s);
	return s;
}

static void core_task(ds_task *t, ds_task_func func, void *param)
{
	dbglog("task: %p %p %p", t, func, param);
	fftaskqueue_post4(&gcore->taskq, t, func, param);
	ffkq_post(gcore->kq, gcore->kq_wake, &gcore->kq_wake_ev);
}

static void core_timer(ds_timer *t, int interval_msec, ds_task_func func, void *param)
{
	dbglog("timer: %p %d %p %p", t, interval_msec, func, param);

	if (interval_msec == 0) {
		fftimerqueue_remove(&gcore->tq, t);
		return;
	}

	fftimerqueue_add(&gcore->tq, t, gcore->now_msec, interval_msec, func, param);
}

static fftime core_clock(ffdatetime *dt, uint flags)
{
	if (flags == DS_CORE_UTC) {
		if (dt)
			fftime_split1(dt, &gcore->utc);
		return gcore->utc;
	}
	return gcore->now;
}

static void tasks_run(void *param)
{
	ffkq_post_consume(gcore->kq, gcore->kq_wake);
	uint n = fftaskqueue_run(&gcore->taskq);
	if (n != 0)
		dbglog("processed %u tasks", n);
}

static void core_exit(int exit_code)
{
	gcore->exit_code = exit_code;
	FFINT_WRITEONCE(gcore->kq_quit, 1);
	ffkq_post(gcore->kq, gcore->kq_wake, &gcore->kq_wake_ev);
}

static void tmr_func(void *param)
{
	fftimer_consume(gcore->tmr);
	fftime_now(&gcore->utc);
	gcore->utc.sec += FFTIME_1970_SECONDS;
	gcore->now = fftime_monotonic();
	gcore->now_msec = fftime_to_msec(&gcore->now);
	uint n = fftimerqueue_process(&gcore->tq, gcore->now_msec);
	if (n != 0)
		dbglog("processed %u timers", n);
}

static ds_core* core_conf(struct ds_core_conf *conf)
{
	gcore->conf = *conf;
	gcore->conf.app_path = ffsz_dup(conf->app_path);
	core->debug = conf->debug;
	core->verbose = conf->debug | conf->verbose;
	core->stdout_color = conf->stdout_color;

	if (FFKQ_NULL == (gcore->kq = ffkq_create())) {
		syserrlog("ffkq_create");
		goto err;
	}
	ds_kevent_set(&gcore->kq_wake_ev, tasks_run, NULL);
	gcore->kq_wake = ffkq_post_attach(gcore->kq, &gcore->kq_wake_ev);
	fftaskqueue_init(&gcore->taskq);

	if (FFTIMER_NULL == (gcore->tmr = fftimer_create(0))) {
		syserrlog("fftimer_create");
		goto err;
	}
	ds_kevent_set(&gcore->tmr_kev, tmr_func, NULL);
	if (0 != fftimer_start(gcore->tmr, gcore->kq, &gcore->tmr_kev, gcore->conf.timer_resolution_msec)) {
		syserrlog("fftimer_start");
		goto err;
	}
	dbglog("sys timer: %u", gcore->conf.timer_resolution_msec);
	fftimerqueue_init(&gcore->tq);
	tmr_func(NULL);
	return &_ds_core;

err:
	return NULL;
}

static int core_run()
{
	dbglog("started worker thread");

	ffkq_time kqt;
	ffkq_time_set(&kqt, -1);

	while (ff_likely(!FFINT_READONCE(gcore->kq_quit))) {

		ffkq_event events[64];
		uint n = ffkq_wait(gcore->kq, events, FF_COUNT(events), kqt);

		ds_dbglog("ffkq_wait: %d", n);

		if (ff_unlikely((int)n < 0)) {
#ifdef FF_UNIX
			if (fferr_last() == EINTR)
				continue;
#endif
			syserrlog("ffkq_wait");
			break;
		}

		for (uint i = 0;  i != n;  i++) {
			ffkq_event *ev = &events[i];
			struct ds_kevent *v = ffkq_event_data(ev);
			uint f = ffkq_event_flags(ev);

#ifdef FF_WIN
			f = FFKQ_READ;
#endif

			if (f & FFKQ_READ)
				v->func(v->param);
			// if (f & FFKQ_WRITE)
			// 	v->func(v->param);
		}
	}

	dbglog("exit worker thread: %u", gcore->exit_code);
	return gcore->exit_code;
}

static void core_init()
{
	DS_ASSERT(gcore == NULL);
	gcore = ffmem_new(struct core);
	gcore->kq = FFKQ_NULL;
	gcore->tmr = FFTIMER_NULL;
	fftime_local(&core->tz);
}

static void core_destroy()
{
	if (gcore == NULL)
		return;

	if (gcore->tmr != FFTIMER_NULL)
		fftimer_close(gcore->tmr, gcore->kq);
	if (gcore->kq != FFKQ_NULL)
		ffkq_close(gcore->kq);
	ffmem_free(gcore->conf.app_path);
	ffmem_free(gcore);  gcore = NULL;
}

extern const ds_file_if _ds_file;
ds_core _ds_core = {
	&_ds_file,
	&sync_if,
	.version_str = DS_VER,
	.exit = core_exit,
	core_path,
	core_task,
	core_timer,
	core_clock,
	core_log, core_logv,
};

FF_EXPORT const struct ds_coreinit ds_coreinit = {
	core_init,
	core_destroy,
	core_conf,
	core_run,
};
