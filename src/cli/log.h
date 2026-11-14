/** dsynco: logger
2022, Simon Zolin */

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

	if ((flags & 0x0f) > DS_LOG_WARN && !x->args.debug)
		flags |= ZZLOG_NO_LEVEL;
	uint64 tid = 0;
	zzlog_printv(&x->log, flags, (x->args.debug) ? x->log_date : NULL, tid, NULL, NULL, fmt, args);
}

void exe_log(uint flags, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	exe_logv(flags, fmt, args);
	va_end(args);
}

static void log_init(struct main *x)
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
