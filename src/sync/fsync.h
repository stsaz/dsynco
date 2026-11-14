/** dsynco: sync: perform directory synchronization
2022, Simon Zolin */

/** Prepare target full file name */
static ffstr out_name(ffstr lname, ffstr lbase, ffstr rbase)
{
	// `in/d/f` -> `out/d/f`
	ffstr_shift(&lname, lbase.len);
	if (lname.len && ffpath_slash(lname.ptr[0]))
		ffstr_shift(&lname, 1);
	ffstr s = {};
	ffsize cap = 0;
	ffstr_growfmt(&s, &cap, "%S%c%S%Z", &rbase, FFPATH_SLASH, &lname);
	return s;
}

static void sync_f_copy(const char *src, char *dst, uint flags, uint overwrite
	, void(*on_complete)(void*, int), void *param)
{
	struct ds_copy_conf c = {};

	struct ds_copy_io *cio = ffvec_pushT(&c.in_out, struct ds_copy_io);
	cio->in = ffsz_dup(src);
	cio->out = dst;

	c.update = !!(flags & DS_SYNC_SYNC_REPLACE_DATE);
	c.replace_date = !!(flags & DS_SYNC_SYNC_REPLACE_DATE);
	c.atomic = !!(flags & DS_SYNC_SYNC_ATOMIC);
	c.overwrite = overwrite;
	c.test = !!(flags & DS_SYNC_SYNC_TEST);
	c.directio = 1;

	c.on_complete = on_complete;
	c.opaque = param;
	ds_dbglog("sync: copy: '%S' -> '%S'", &src, &dst);
	ds_copy_run(&c);
}

static void sync_f_del(ffvec *input, uint flags
	, void(*on_complete)(void*, int), void *param)
{
	struct ds_trash_conf c = {};
	c.input = *input;
	c.delete_on_fail = 1;
	c.test = !!(flags & DS_SYNC_SYNC_TEST);
	c.on_complete = on_complete;
	c.opaque = param;
	ds_trash_run(&c);
}

static void op_complete(void *param, int result)
{
	ds_sync_diff *sd = param;
	sd->sync.result = result;
	sd->sync.have_result = 1;
	sd->sync.on_event(sd->sync.udata);
}

/**
Return
	0: success
	1: async; op_complete() will be called
	-1: error */
int sync_sync1(ds_sync_diff *sd, void *diff_entry_id, uint flags
	, void(*on_complete)(void*, int), void *param)
{
	int rc = -1;
	ffvec v = {};
	struct ds_sync_diff_entry de_s, *de = &de_s;
	sync_info(sd, diff_entry_id, flags & DS_SYNC_INFO_SWAP, de);
	ds_sync_snapshot *l = sd->left, *r = sd->right;
	if (flags & DS_SYNC_SWAP) {
		FF_SWAP2(l, r);
	}

	switch (de->status & DS_SYNC_MASK) {
	case DS_SYNC_LEFT:
	case DS_SYNC_NEQ: {
		char *dst = out_name(de->lname, l->root_dir, r->root_dir).ptr;
		sync_f_copy(de->lname.ptr, dst, flags, !!(de->status & DS_SYNC_NEQ), on_complete, param);
		rc = 1;
		goto end;
	}

	case DS_SYNC_RIGHT: {
		ffvec input = {};
		ffstr *p = ffvec_pushT(&input, ffstr);
		char *sz = ffsz_dupstr(&de->rname);
		ffstr_setz(p, sz);
		ds_dbglog("sync: %s: '%S'", (flags & DS_SYNC_SYNC_FILE_DEL) ? "del" : "trash", &de->rname);
		if (flags & DS_SYNC_SYNC_FILE_DEL) {
			uint f = (flags & DS_SYNC_SYNC_TEST) ? DS_FILE_DEL_TEST : 0;
			rc = core->file->del(sz, f);
			rc = (!rc) ? 0 : -1;
		} else {
			sync_f_del(&input, flags, on_complete, param);
			rc = 1;
		}
		goto end;
	}

	case DS_SYNC_EQ:
		break;

	case DS_SYNC_MOVE: {
		if (flags & DS_SYNC_SYNC_REPLACE_DATE) {
			ds_errlog("DS_SYNC_SYNC_REPLACE_DATE is not supported for 'moved' status");
			goto end;
		}

		// "right_root/left_subpath/left_name"
		ffstr lname = de->lname;
		if (lname.len && ffpath_slash(lname.ptr[0]))
			ffstr_shift(&lname, 1);
		ffstr_shift(&lname, l->root_dir.len);
		ffvec_addfmt(&v, "%S%c%S"
			, &r->root_dir, FFPATH_SLASH, &lname);
		uint mf = DS_FILE_MOVE_SAFE;
		if (flags & DS_SYNC_SYNC_TEST)
			mf |= DS_FILE_MOVE_TEST;
		if (core->file->move(de->rname, ffvec_str(&v), mf))
			goto end;
		break;
	}

	default:
		DS_ASSERT(0);
		goto end;
	}

	rc = 0;

end:
	ffvec_free(&v);
	ds_sync_diff_entry_destroy(de);
	return rc;
}

int sync_sync(ds_sync_diff *sd, void* diff_entry_ids[], int result[], uint n, uint flags
	, void (*on_event)(void*), void *param)
{
	if (sd->sync.on_event) {
		if (sd->sync.have_result) {
			DS_ASSERT(n);
			sd->sync.have_result = 0;
			result[0] = sd->sync.result;
			sd->sync.on_event = NULL;
			return 1;
		}
		return -1; // Waiting for on_event()
	}

	uint done = 0;
	int r;
	for (uint i = 0;  i < n;  i++) {
		r = sync_sync1(sd, diff_entry_ids[i], flags, op_complete, sd);
		if (r > 0) {
			sd->sync.on_event = on_event;
			sd->sync.udata = param;
			if (!done)
				return -1; // Waiting for on_event()
			// Return ASYNC code next time
			return done;
		}
		result[done++] = r;
	}
	return done;
}
