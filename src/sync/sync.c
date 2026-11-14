/** dsynco: diff/synchronize directories
2022, Simon Zolin */

#include <dsynco.h>
#include <ffbase/fntree.h>
#include <ffsys/path.h>
#include <ffbase/map.h>

extern const ds_core *core;

#include <sync/scan.h>
#include <sync/diff.h>
#include <sync/fsync.h>

static ds_sync_snapshot* sync_scan2(ffstr path, uint flags)
{
	if (flags & DS_SYNC_SCAN_WILDCARD)
		return sync_scan_wc(path, flags);
	return sync_scan(path, flags);
}

const struct ds_sync_if sync_if = {
	sync_scan2,
	sync_scan_next,
	sync_snapshot_free,
	sync_diff,
	sync_diff_next,
	sync_find_dups,
	sync_diff_free,
	sync_view,
	sync_sort,
	sync_info,
	sync_status,
	sync_sync,
};


struct sync {
	struct ds_sync_conf conf;
	ds_task task;

	uint st;
	const ds_sync_if *sync_if;
	struct snapshot	*src, *dst;
	ffstr			dir;
	struct ent		ent;
	uint			cmp_idx;
	uint			stop;
	uint			hdr :1;
	uint			ent_ready :1;
	uint			diff_only :1;

	struct diff cmp;

	struct {
		ffvec batch; // struct fntree_cmp_ent*[]
		ffvec result; // int[]
		uint batch_idx;
		struct {
			uint add, del, replaced, moved, err;
		} stats;
	} sc;

	u_char	left_path_strip, right_path_strip;
	fftime	since_time;
};

static void sync_destroy(struct sync *s)
{
	if (s->src)
		snapshot_close(s->src);
	ffmem_free(s->src);
	if (s->dst)
		snapshot_close(s->dst);
	ffmem_free(s->dst);
	ffvec_free(&s->sc.batch);
	ffvec_free(&s->sc.result);
}

static int sync_left_tree_init(struct sync *s)
{
	s->src = ffmem_new(struct snapshot);
	ffstr rtpath = {};
	s->src->root = fntree_create(rtpath);
	ffstr *it;
	FFSLICE_WALK(&s->conf.input, it) {
		if (NULL == fntree_add(&s->src->root, *it, sizeof(struct ds_sync_entry)))
			return -1;
	}
	if (s->conf.input.len == 1) {
		ffstr *path = ffslice_itemT(&s->conf.input, 0, ffstr);
		ffstr_dupstr(&s->src->root_dir, path);
	}

	return 0;
}

static int sync_right_tree_init(struct sync *s)
{
	s->dst = ffmem_new(struct snapshot);
	ffstr rtpath = {};
	s->dst->root = fntree_create(rtpath);
	if (NULL == fntree_add(&s->dst->root, s->conf.output, sizeof(struct ds_sync_entry)))
		return -1;

	ffstr_dupstr(&s->dst->root_dir, &s->conf.output);

	return 0;
}

static void sync_diff_begin(struct sync *s)
{
	ds_infolog("Comparing source & target...");
	uint flags = 0;
	if (s->left_path_strip)
		flags |= DS_SYNC_DIFF_LEFT_PATH_STRIP;
	if (s->right_path_strip)
		flags |= DS_SYNC_DIFF_RIGHT_PATH_STRIP;
	if (s->conf.diff_no_attr)
		flags |= DS_SYNC_DIFF_NO_ATTR;
	if (s->conf.diff_no_time)
		flags |= DS_SYNC_DIFF_NO_TIME;
	if (s->conf.diff_time_2sec)
		flags |= DS_SYNC_DIFF_TIME_2SEC;
	diff_init(&s->cmp, s->src, s->dst, flags);
}

static void sync_diff_fin(struct sync *s)
{
	ds_infolog("diff status: add:%u  moved:%u  upd:%u  del:%u  eq:%u  total:%u/%u"
		, s->cmp.stats.left, s->cmp.stats.moved, s->cmp.stats.neq, s->cmp.stats.right, s->cmp.stats.eq
		, s->cmp.stats.ltotal, s->cmp.stats.rtotal);

	struct ds_sync_props props = {};
	props.include = *(ffslice*)&s->conf.include;
	props.exclude = *(ffslice*)&s->conf.exclude;
	props.since_time = s->since_time;

	uint show_flags = s->conf.diff_flags;
	s->sync_if->view(&s->cmp, &props, show_flags);
}

/* Format:
"STATUS LSIZE LNAME OPERATOR RSIZE RNAME"

Examples:
	ADD       5  /l         >>
	DEL                     X   6  /r
	MOV       6  /mv        ->  6  /d/mv
	UPD[O<.]  8  /mod       <   9  /mod
	UPD[N..]  9  /mod_date  >=  9  /mod_date
*/
static void sync_status_print(struct sync *s, const struct ds_sync_diff_entry *de, ffstr lname, ffstr rname)
{
	ffvec v = {};
	const char *op = NULL;

	if (core->stdout_color) {
		const char *clr = NULL;
		switch (de->status & DS_SYNC_MASK) {
		case DS_SYNC_LEFT:
			clr = FFSTD_CLR_I(FFSTD_GREEN);  break;
		case DS_SYNC_RIGHT:
			clr = FFSTD_CLR_I(FFSTD_RED);  break;
		case DS_SYNC_MOVE:
			clr = FFSTD_CLR_I(FFSTD_BLUE);  break;
		}
		if (clr)
			ffvec_addsz(&v, clr);
	}

	if (!s->conf.diff_full_name && s->src->root_dir.len) {
		if (lname.len)
			ffstr_shift(&lname, s->src->root_dir.len);
		if (rname.len)
			ffstr_shift(&rname, s->conf.output.len);
	}

	switch (de->status & DS_SYNC_MASK) {
	case DS_SYNC_LEFT:
		ffvec_addsz(&v, "ADD       ");
		op = ">>";
		break;

	case DS_SYNC_RIGHT:
		ffvec_addfmt(&v, "DEL       %*c", 10L + 1 + 40L, ' ');
		op = "X ";
		break;

	case DS_SYNC_MOVE:
		ffvec_addsz(&v, "MOV       ");
		op = "->";
		break;

	case DS_SYNC_NEQ: {
		int a1 = '.', a2 = '.', a3 = '.';
		uint st = de->status;

		if (st & DS_SYNC_NEWER)
			a1 = 'N';
		else if (st & DS_SYNC_OLDER)
			a1 = 'O';

		if (st & DS_SYNC_LARGER)
			a2 = '>';
		else if (st & DS_SYNC_SMALLER)
			a2 = '<';

		if (st & DS_SYNC_ATTR)
			a3 = 'A';

		ffvec_addfmt(&v, "UPD[%c%c%c]  ", a1, a2, a3);

		op = "!=";
		if (st & DS_SYNC_LARGER)
			op = "> ";
		else if (st & DS_SYNC_SMALLER)
			op = "< ";
		else if (st & DS_SYNC_NEWER)
			op = ">=";
		else if (st & DS_SYNC_OLDER)
			op = "<=";
		break;
	}
	}

	if (de->left) {
		ffvec_addfmt(&v, "%*U %*S"
			, 10L, de->left->size
			, 40L, &lname);
	}

	ffvec_addfmt(&v, "  %s  ", op);

	if (de->right) {
		ffvec_addfmt(&v, "%*U %S"
			, 10L, de->right->size
			, &rname);
	}

	if (core->stdout_color)
		ffvec_addsz(&v, FFSTD_CLR_RESET);

	ds_infolog("%S", &v);
	ffvec_free(&v);
}

/** Show next difference from 'cmp.ents' */
static int sync_diff_show_next(struct sync *s)
{
	int rc = 1;
	struct ds_sync_diff_entry de_s, *de = &de_s;
	if (s->sync_if->info(&s->cmp, (void*)(size_t)s->cmp_idx++, DS_SYNC_INFO_INDEX, de))
		return 1;

	if (s->conf.plain_list) {
		switch (de->status & DS_SYNC_MASK) {
		case DS_SYNC_LEFT:
			ffstdout_fmt("%S\n", &de->lname);  break;

		case DS_SYNC_RIGHT:
			ffstdout_fmt("%S\n", &de->rname);  break;

		case DS_SYNC_NEQ:
			ffstdout_fmt("%S\n", &de->lname);  break;

		case DS_SYNC_MOVE:
			ffstdout_fmt("%S\n", &de->lname);  break;
		}
		rc = 0;
		goto end;
	}

	switch (de->status & DS_SYNC_MASK) {
	case DS_SYNC_LEFT:
	case DS_SYNC_RIGHT:
	case DS_SYNC_NEQ:
	case DS_SYNC_MOVE:
		sync_status_print(s, de, de->lname, de->rname);
		break;

	case DS_SYNC_EQ:
		break;

	default:
		DS_ASSERT(0);
		goto end;
	}

	rc = 0;

end:
	ds_sync_diff_entry_destroy(de);
	return rc;
}

static void sync_run(void *param);

static void sync_close(struct sync *s, int rc)
{
	void (*on_complete)(void*, int) = s->conf.on_complete;
	void *udata = s->conf.opaque;
	sync_destroy(s);
	ffmem_free(s);
	ds_dbglog("sync: calling on_complete(): %d", rc);
	on_complete(udata, rc);
}

static int sync_prep(struct sync *s)
{
	struct fntree_cmp_ent *it;
	FFSLICE_WALK(&s->cmp.ents, it) {
		struct fntree_cmp_ent *ce = it;
		if (ce->status & _DS_SYNC_SKIP)
			continue;

		uint do_sync = 0;
		switch (ce->status & DS_SYNC_MASK) {
		case DS_SYNC_LEFT:
			do_sync = s->conf.sync_add;
			break;
		case DS_SYNC_RIGHT:
			do_sync = s->conf.sync_del;
			break;
		case DS_SYNC_NEQ: {
			const struct ds_sync_entry *ld = (void*)fntree_data(ce->l);
			if (ld->unix_attr & FFFILE_UNIX_DIR)
				continue; // Skip dirs
			do_sync = s->conf.sync_update;
			break;
		}
		case DS_SYNC_MOVE:
			do_sync = s->conf.sync_move;
			break;
		case DS_SYNC_EQ:
			continue;
		default:
			DS_ASSERT(0);
			return 'e';
		}

		if (do_sync)
			*ffvec_pushT(&s->sc.batch, void*) = ce;
	}

	ffvec_free(&s->sc.result);
	ffvec_allocT(&s->sc.result, s->sc.batch.len, int);
	return 0;
}

static void sync_result(struct sync *s, uint i, int res)
{
	const struct fntree_cmp_ent *ce = *ffslice_itemT(&s->sc.batch, i, struct fntree_cmp_ent*);

	if (res) {
		s->sc.stats.err++;
		return;
	}

	switch (ce->status & DS_SYNC_MASK) {
	case DS_SYNC_LEFT:
		s->sc.stats.add++;  break;
	case DS_SYNC_NEQ:
		s->sc.stats.replaced++;  break;
	case  DS_SYNC_MOVE:
		s->sc.stats.moved++;  break;
	case DS_SYNC_RIGHT:
		s->sc.stats.del++;  break;
	}
}

/** Perform a single sync operation */
static int sync1(struct sync *s)
{
	if (s->sc.batch_idx == s->sc.batch.len) {
		ds_infolog("Result: added:%u  moved:%u  replaced:%u  deleted:%u  err:%u"
			, s->sc.stats.add, s->sc.stats.moved, s->sc.stats.replaced, s->sc.stats.del, s->sc.stats.err);
		return 'd';
	}

	uint flags = 0;
	if (s->conf.replace_date)
		flags |= DS_SYNC_SYNC_REPLACE_DATE;
	if (s->conf.atomic)
		flags |= DS_SYNC_SYNC_ATOMIC;
	if (s->conf.test)
		flags |= DS_SYNC_SYNC_TEST;

	uint n = s->sc.batch.len - s->sc.batch_idx;
	int r = sync_sync(&s->cmp, (void**)s->sc.batch.ptr + s->sc.batch_idx, s->sc.result.ptr, n, flags, sync_run, s);
	if (r < 0)
		return 'a';
	for (uint i = 0;  i < (uint)r;  i++) {
		int res = *ffslice_itemT(&s->sc.result, i, int);
		sync_result(s, s->sc.batch_idx + i, res);
	}
	s->sc.batch_idx += r;
	return 0;
}

static void sync_run(void *param)
{
	struct sync *s = (struct sync*)param;
	int r, rc = 1;
	enum {
		I_INIT, I_IN,
		I_OUT_INIT, I_OUT,
		I_DIFF_BEGIN, I_DIFF, I_DIFF_SHOW,
		I_SYNC_PREP, I_SYNC,
	};

	while (1) {
		switch (s->st) {
		case I_INIT:
			if (sync_left_tree_init(s)) goto end;
			ds_infolog("Scanning source...");
			s->st = I_IN;
			// fallthrough

		case I_IN:
			switch (snapshot_scan_next(s->src, &s->ent)) {
			case 'nblk':
				break;

			case 'done':
				s->st = I_OUT_INIT;
				continue;

			case 'erro':
				continue;
			}

			s->ent_ready = 1;
			if (s->hdr) {
				s->dir = s->src->path;
				s->ent.name = s->src->name_segment;
			}
			continue;

		case I_OUT_INIT:
			if (sync_right_tree_init(s)) goto end;
			ds_infolog("Scanning target...");
			s->st = I_OUT;
			// fallthrough

		case I_OUT:
			switch (snapshot_scan_next(s->dst, NULL)) {
			case 'done':
				s->st = I_DIFF_BEGIN;
				continue;

			case 'erro':
				continue;
			}

			continue;

		case I_DIFF_BEGIN:
			sync_diff_begin(s);
			s->st = I_DIFF;
			// fallthrough

		case I_DIFF:
			if (diff_next(&s->cmp)) {
				sync_diff_fin(s);
				s->st = I_DIFF_SHOW;
			}
			continue;

		case I_DIFF_SHOW:
			if (0 != sync_diff_show_next(s)) {
				if (s->diff_only) {
					rc = 0;
					goto end;
				}
				s->st = I_SYNC_PREP;
			}
			continue;

		case I_SYNC_PREP:
			if (sync_prep(s))
				goto end;
			s->st = I_SYNC;
			// fallthrough

		case I_SYNC:
			if ((r = sync1(s))) {
				if (r == 'a')
					return;
				rc = (s->sc.stats.err) ? 1 : 0;
				goto end;
			}
			continue;
		}
	}

end:
	sync_close(s, rc);
}

FF_EXTERN int ds_sync_run(const struct ds_sync_conf *conf)
{
	struct sync *s = ffmem_new(struct sync);
	s->conf = *conf;
	s->sync_if = &sync_if;

	s->left_path_strip = s->right_path_strip = 1;

	if (s->conf.diff_flags)
		s->diff_only = 1;

	if (s->conf.plain_list && !s->diff_only) {
		ds_errlog("'plain' requires 'diff'");
		return -1;
	}

	if (!conf->output.len)
		return -1;

	if (conf->recent_days) {
		s->since_time = core->clock(NULL, DS_CORE_UTC);
		s->since_time.sec -= conf->recent_days * 60*60*24;
	}

	core->task(&s->task, sync_run, s);
	return 0;
}
