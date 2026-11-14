/** dsynco: GUI
2024, Simon Zolin */

#ifdef _WIN32
	#include <util/windows-shell.h>
#endif

#include <dsynco.h>
#include <util/util.hpp>
#ifdef FF_WIN
	#include <ffgui/winapi/loader.h>
#else
	#include <util/unix-shell.h>
	#include <ffgui/gtk/loader.h>
#endif
#include <ffgui/loader.h>
#include <ffgui/gui.hpp>
#include <ffsys/thread.h>
#include <ffsys/path.h>
#include <ffsys/globals.h>
#include <ffsys/process.h>

static const ds_core *core;

#define INCLUDE_ACTIONS(_) \
	_(A_WND_NEW), \
	_(A_SCAN_CMP), \
	_(A_SCAN_DUP), \
	_(A_SYNC), \
	_(A_SYNC_DATE), \
	_(A_SWAP), \
	_(A_DIFF_NO_DATE), \
	_(A_DIFF_MOVE_NO_NAME), \
	_(A_SYNC_FDEL), \
	_(A_SORT_FILESIZE), \
	_(A_SORT_FILEDATE), \
	_(A_SHOW_INC_BOTH), \
	_(A_INCLUDE_CHANGE), \
	_(A_EXCLUDE_CHANGE), \
	_(A_RECENTDAYS_CHANGE), \
	_(A_SHOW_LEFT), \
	_(A_SHOW_EQ), \
	_(A_SHOW_NEQ), \
	_(A_SHOW_MOV), \
	_(A_SHOW_RIGHT), \
	_(A_SHOW_DONE), \
	_(A_SHOW_NEQ_DATE), \
	_(A_SRC_EXEC), \
	_(A_DST_EXEC), \
	_(A_SRC_SHOW_DIR), \
	_(A_DST_SHOW_DIR), \
	_(A_SRC_DELETE), \
	_(A_DST_DELETE), \
	_(A_FILTER_NAME), \
	_(A_SEL_ALL), \
	_(A_LIST_DISPLAY), \
	_(A_QUIT), \
	_(A_ONCLOSE),

#define _(id) id
enum {
	A_NONE,
	INCLUDE_ACTIONS(_)
};
#undef _

enum {
	COL_STATUS,
	COL_LDIR,
	COL_LNAME,
	COL_RDIR,
	COL_RNAME,
	COL_LSIZE,
	COL_RSIZE,
	COL_LDATE,
	COL_RDATE,
};

static int bit_toggle(uint *i, uint f)
{
	*i ^= f;
	return !!(*i & f);
}

static ffstr fftime_to_str(fftime t, xxstr_buf<100>& buf, uint flags)
{
	ffdatetime dt;
	fftime_split1(&dt, &t);
	buf.len = fftime_tostr1(&dt, buf.ptr, 100, flags);
	return buf;
}

/** "A;B" -> ["A", "B"] */
static ffslice str_split_array(ffstr s, char by, xxvec *dst)
{
	dst->len = 0;
	while (s.len) {
		ffstr it;
		ffstr_splitby(&s, by, &it, &s);
		if (it.len)
			*dst->push<ffstr>() = it;
	}
	return dst->slice();
}

struct gsync {
	const ds_sync_if *sync_if;
	const char *dir_src, *dir_dst;
	ds_sync_snapshot *lsnap, *rsnap;
	ds_sync_diff *diff;
	struct ds_sync_props props;
	struct ds_sync_diff_entry de;

	bool		wildcard;
	uint		scan_state;
	uint		scan_flags;
	uint		show_flags; // enum DS_SYNC
	xxvec		include, exclude; // ffstr[]
	xxvec		include_str, exclude_str;
	ds_timer	timer, sync_timer;
	xxvec		sync_ids, sync_items, result;
	uint		sync_item_i;
	ds_task	core_task;
	uint		diff_flags, sync_flags, sync_flags_add;
	xxvec		sel_indices; // uint[]

	ffthread	thread;
	ffui_menuxx	mfile, mlist, mpopup;
	struct wmain {
		ffui_windowxx	wnd;
		ffui_menu		mmenu;
		ffui_labelxx	llpath, lrpath;
		ffui_editxx		lpath, rpath;
		ffui_labelxx	linclude, lexclude, ldays;
		ffui_editxx		include, exclude, recent_days;
		ffui_checkboxxx	show_left, show_eq, show_neq, show_mov, show_right, show_done, show_neq_date;
		ffui_viewxx		view;
		ffui_statusbarxx stbar;
	} wmain;

	struct {
		ushort	status_update_period;
	} conf;

	~gsync() {
		diff_reset();
		ds_sync_diff_entry_destroy(&this->de);
	}

	void error(const char *e)
	{
		ds_errlog(e);
		this->wmain.stbar.text(0, e);
	}

	void timer_set(int interval_msec, void (*func)(gsync *g)) {
		core->timer(&this->sync_timer, interval_msec, (ds_task_func)func, this);
	}

	void diff_reset()
	{
		this->sync_if->diff_free(this->diff);
		this->diff = NULL;
		this->sync_if->snapshot_free(this->lsnap);
		this->lsnap = NULL;
		this->sync_if->snapshot_free(this->rsnap);
		this->rsnap = NULL;
		this->show_flags &= ~DS_SYNC_SWAP;
	}

	void redraw()
	{
		if (!this->diff) return;

		this->props.include = str_split_array(this->include_str.acquire(this->wmain.include.text()).str(), ';', &this->include);
		this->props.exclude = str_split_array(this->exclude_str.acquire(this->wmain.exclude.text()).str(), ';', &this->exclude);

		this->props.since_time.sec = 0;
		uint days = xxvec(this->wmain.recent_days.text()).str().uint16(0);
		if (days) {
			this->props.since_time = core->clock(NULL, DS_CORE_UTC);
			this->props.since_time.sec -= days * 60*60*24;
		}

		uint n = this->sync_if->view(this->diff, &this->props, this->show_flags);

		this->wmain.view.length(n, 1);

		xxstr_buf<100> buf;
		this->wmain.stbar.text(0, buf.zfmt("%u / %u", n, this->props.stats.entries));
	}

	void stats_redraw()
	{
		uint l = this->props.stats.left;
		uint r = this->props.stats.right;
		if (this->show_flags & DS_SYNC_SWAP) {
			l = this->props.stats.right;
			r = this->props.stats.left;
		}

		xxstr_buf<100> buf;
		this->wmain.show_left.text(buf.zfmt("%s (%u)"
			, "LEFT", l));
		this->wmain.show_eq.text(buf.zfmt("%s (%u)"
			, "EQ", this->props.stats.eq));
		this->wmain.show_neq.text(buf.zfmt("%s (%u)"
			, "NEQ", this->props.stats.neq));
		this->wmain.show_mov.text(buf.zfmt("%s (%u)"
			, "MOV", this->props.stats.moved));
		this->wmain.show_right.text(buf.zfmt("%s (%u)"
			, "RIGHT", r));
	}

	ds_sync_snapshot* scan(xxstr path)
	{
		uint flags = 0;
		this->wildcard = (path.find_char('*') >= 0);
		if (this->wildcard)
			flags |= DS_SYNC_SCAN_WILDCARD;
		else
			flags |= DS_SYNC_SCAN_STEP;
		return this->sync_if->scan(path, flags);
	}

	static void scan_status_update__wk(gsync *g) {
		ffui_thd_post(scan_status_update, g);
	}

	enum { I_INIT, I_SCAN, I_SCAN_NEXT, I_DIFF, I_DIFF_NEXT, };
	static void scan_and_compare__worker(void *param)
	{
		gsync *g = (gsync*)param;

		switch (g->scan_state) {
		case I_INIT:
			g->diff_reset();
			ffmem_zero_obj(&g->props.stats);
			g->stats_redraw();
			g->scan_flags = 0;
			g->scan_state++;
			// fallthrough

		case I_SCAN:
			if (!(g->lsnap = g->scan(xxvec(g->wmain.lpath.text()).str()))) {
				g->error("ERROR scanning Source tree");
				goto err;
			}
			if (!(g->rsnap = g->scan(xxvec(g->wmain.rpath.text()).str()))) {
				g->error("ERROR scanning Target tree");
				goto err;
			}
			g->timer_set(g->conf.status_update_period, scan_status_update__wk);
			if (g->wildcard) {
				g->scan_state = I_DIFF;
				goto i_scan_diff;
			}
			ffui_thd_post(scan_status_update, g);
			g->scan_state++;
			// fallthrough

		case I_SCAN_NEXT:
			for (uint i = 0;;  i++) {
				if (i == 100) {
					g->core_task_add(scan_and_compare__worker);
					return;
				}

				if (!(g->scan_flags & 1)) {
					if (!g->sync_if->scan_next(g->lsnap))
						g->props.stats.ltotal++;
					else
						g->scan_flags |= 1;
				}

				if (!(g->scan_flags & 2)) {
					if (!g->sync_if->scan_next(g->rsnap))
						g->props.stats.rtotal++;
					else
						g->scan_flags |= 2;
				}

				if ((g->scan_flags & 3) == 3)
					break;
			}

			ffui_thd_post(scan_status_update, g);
			g->scan_state++;
			// fallthrough

		i_scan_diff:
		case I_DIFF: {
			ffui_thd_post(scan_status_update, g);
			uint f = DS_SYNC_DIFF_LEFT_PATH_STRIP
				| DS_SYNC_DIFF_RIGHT_PATH_STRIP
				| DS_SYNC_DIFF_NO_ATTR
				| DS_SYNC_DIFF_TIME_2SEC
				| DS_SYNC_DIFF_STEP;
			f |= g->diff_flags;
			f |= (f & DS_SYNC_DIFF_MOVE_NO_NAME) ? DS_SYNC_DIFF_MOVE_CHK_CONTENT : 0;
			g->diff = g->sync_if->diff(g->lsnap, g->rsnap, &g->props, f);
		}
			g->scan_state++;
			// fallthrough

		case I_DIFF_NEXT:
			for (uint i = 0;;  i++) {
				if (i == 100) {
					g->core_task_add(scan_and_compare__worker);
					return;
				}
				if (g->sync_if->diff_next(g->diff, &g->props))
					break;
			}

			g->timer_set(0, NULL);
			ffui_thd_post(scan_status_update, g);
			g->stats_redraw();
			g->redraw();
			return;
		}

err:
		g->timer_set(0, NULL);
	}

	static void dups_status_update__wk(gsync *g) {
		ffui_thd_post(scan_status_update, g);
	}

	static void scan_dups__worker(void *param)
	{
		gsync *g = (gsync*)param;

		switch (g->scan_state) {
		case I_INIT:
			g->diff_reset();
			ffmem_zero_obj(&g->props.stats);
			g->stats_redraw();
			g->scan_state++;
			// fallthrough

		case I_SCAN:
			ffui_thd_post(scan_status_update, g);
			if (!(g->lsnap = g->scan(xxvec(g->wmain.lpath.text()).str()))) {
				g->error("ERROR scanning Source tree");
				return;
			}
			g->timer_set(g->conf.status_update_period, dups_status_update__wk);
			if (g->wildcard) {
				g->scan_state = I_DIFF;
				goto i_diff;
			}
			g->scan_state++;
			// fallthrough

		case I_SCAN_NEXT:
			for (uint i = 0;;  i++) {
				if (i == 100) {
					g->core_task_add(scan_dups__worker);
					return;
				}
				if (g->sync_if->scan_next(g->lsnap))
					break;
				g->props.stats.ltotal++;
			}

			ffui_thd_post(scan_status_update, g);
			g->scan_state++;
			// fallthrough

		i_diff:
		case I_DIFF:
			ffui_thd_post(scan_status_update, g);
			g->diff = g->sync_if->find_dups(g->lsnap, &g->props, DS_SYNC_DIFF_STEP);
			g->scan_state++;
			// fallthrough

		case I_DIFF_NEXT:
			for (uint i = 0;;  i++) {
				if (i == 100) {
					g->core_task_add(scan_dups__worker);
					return;
				}
				if (g->sync_if->diff_next(g->diff, &g->props))
					break;
			}

			g->timer_set(0, NULL);
			ffui_thd_post(scan_status_update, g);
			g->stats_redraw();
			g->redraw();
		}
	}

	static const char* entry_status_str(xxstr_buf<100> *buf, uint status)
	{
		if (status & DS_SYNC_SYNCING)
			return "...";

		if (status & DS_SYNC_ERROR)
			return "ERROR";

		if (status & DS_SYNC_DONE)
			return "DONE";

		switch (status & DS_SYNC_MASK) {
		case DS_SYNC_LEFT:
			return "LEFT";

		case DS_SYNC_RIGHT:
			return "RIGHT";

		case DS_SYNC_NEQ: {
			int as = '.', ad = '.', aa = '.';
			if (status & DS_SYNC_NEWER)
				as = 'N';
			else if (status & DS_SYNC_OLDER)
				as = 'O';

			if (status & DS_SYNC_LARGER)
				ad = '>';
			else if (status & DS_SYNC_SMALLER)
				ad = '<';

			if (status & DS_SYNC_ATTR)
				aa = 'A';

			return buf->zfmt("NEQ[%c%c%c]", as, ad, aa);
		}

		case DS_SYNC_MOVE:
			return "MOVE";

		case DS_SYNC_EQ:
			return "EQ";
		}
		return NULL;
	}

	const struct ds_sync_diff_entry* entry_at(uint i)
	{
		uint f = DS_SYNC_INFO_INDEX | (this->show_flags & DS_SYNC_INFO_SWAP);
		ds_sync_diff_entry_destroy(&this->de);
		this->sync_if->info(this->diff, (void*)(size_t)i, f, &this->de);
		return &this->de;
	}

	void list_cell_draw(ffui_view_disp *disp)
	{
		if (!this->diff) return;

	#ifdef FF_WIN
		if (!(disp->mask & LVIF_TEXT))
			return;
	#endif

		const struct ds_sync_diff_entry *ent = this->entry_at(ffui_view_dispinfo_index(disp));

		ffstr s = {};
		xxstr_buf<100> buf;

		switch (ffui_view_dispinfo_subindex(disp)) {
		case COL_STATUS:
			s = FFSTR_Z(entry_status_str(&buf, ent->status)); break;

		case COL_LDIR:
			ffpath_splitpath_str(ent->lname, &s, NULL); break;

		case COL_RDIR:
			ffpath_splitpath_str(ent->rname, &s, NULL); break;

		case COL_LNAME:
			ffpath_splitpath_str(ent->lname, NULL, &s); break;

		case COL_RNAME:
			ffpath_splitpath_str(ent->rname, NULL, &s); break;

		case COL_LSIZE:
			if (ent->left)
				s = buf.fmt("%,U", ent->left->size);
			break;

		case COL_RSIZE:
			if (ent->right)
				s = buf.fmt("%,U", ent->right->size);
			break;

		case COL_LDATE:
			if (ent->left)
				s = fftime_to_str(ent->left->mtime, buf, FFTIME_YMD);
			break;

		case COL_RDATE:
			if (ent->right)
				s = fftime_to_str(ent->right->mtime, buf, FFTIME_YMD);
			break;
		}

		if (s.len)
			ffui_view_dispinfo_settext(disp, s.ptr, s.len);
	}

	void src_dst_swap()
	{
		xxvec lp = this->wmain.lpath.text();
		this->wmain.lpath.text(this->wmain.rpath.text());
		this->wmain.rpath.text(lp.str());
		bit_toggle(&this->show_flags, DS_SYNC_SWAP);
		this->stats_redraw();
		this->redraw();
	}

	void sync_begin(uint flags)
	{
		if (this->sync_items.len) {
			this->error("sync already in progress");
			return;
		}

		this->sync_flags = flags;
		this->sync_prepare();
		if (this->sync_items.len) {
			this->result.free();
			this->result.alloc<int>(this->sync_items.len);
			sync_status_update(this);
			this->core_task_add(sync__worker);
		}
	}

	void sync_prepare()
	{
		xxvec indices = this->wmain.view.selected();
		uint *it;
		FFSLICE_WALK(&indices, it) {
			const struct ds_sync_diff_entry *de = this->entry_at(*it);
			if (!(de->status & DS_SYNC_DONE)) {
				*this->sync_ids.push<void*>() = de->id;
				*this->sync_items.push<uint>() = *it;
			}
		}
		this->sync_item_i = 0;
	}

	static void scan_status_update(void *param)
	{
		struct gsync *g = (gsync*)param;
		xxstr_buf<100> b;
		if (g->scan_state <= I_SCAN_NEXT)
			b.zfmt("Scanning (%u)...", g->props.stats.ltotal + g->props.stats.rtotal);
		else
			b.zfmt("Comparing (%u)...", g->props.stats.entries);
		g->wmain.stbar.text(0, b.buf);
	}

	static void sync_status_update(void *param)
	{
		struct gsync *g = (gsync*)param;
		g->wmain.stbar.text(0, xxstr_buf<100>().zfmt("Synchronizing: %u / %L"
			, g->sync_item_i, g->sync_items.len));
		g->stats_redraw();
	}

	void sync_entry_status_update__worker(void *id, uint index, uint st)
	{
		uint status = this->sync_if->status(this->diff, id
			, DS_SYNC_SYNCING | DS_SYNC_ERROR | DS_SYNC_DONE, st);

		if (status & DS_SYNC_DONE) {
			if (status & DS_SYNC_LEFT) {
				if (this->show_flags & DS_SYNC_SWAP)
					this->props.stats.right--;
				else
					this->props.stats.left--;
			}
			if (status & DS_SYNC_NEQ)
				this->props.stats.neq--;
			else if (status & DS_SYNC_MOVE)
				this->props.stats.moved--;
		}

		this->wmain.view.update(index, 0);
	}

	static void sync__worker(void *param)
	{
		struct gsync *g = (gsync*)param;
		uint f = g->sync_flags
			| (g->show_flags & DS_SYNC_SWAP);
		while (g->sync_item_i < g->sync_ids.len) {
			uint n = g->sync_ids.len - g->sync_item_i;
			int r = g->sync_if->sync(g->diff, (void**)g->sync_ids.ptr + g->sync_item_i, (int*)g->result.ptr, n, f, sync__worker, g);
			if (r < 0)
				return; // Wait for sync__worker() to be called
			for (uint i = 0;  i < (uint)r;  i++) {
				void *id = *g->sync_ids.at<void*>(g->sync_item_i + i);
				uint res = *g->result.at<int>(i);
				g->sync_entry_status_update__worker(id, g->sync_item_i + i, (!res) ? DS_SYNC_DONE : DS_SYNC_ERROR);
			}
			g->sync_item_i += r;
			ffui_thd_post(sync_status_update, g);
		}

		ffui_thd_post(sync_done, g);
	}

	static void sync_done(void *param)
	{
		struct gsync *g = (gsync*)param;
		g->sync_items.len = 0;
		g->sync_ids.len = 0;
		g->wmain.stbar.text(0, "Sync complete");
	}

	void quit()
	{
		ffui_post_quitloop();
	}

	void show_x(ffui_checkboxxx &cb, uint flag)
	{
		if (cb.checked())
			this->show_flags |= flag;
		else
			this->show_flags &= ~flag;
		this->redraw();
	}

	static void filter_change(void *param)
	{
		struct gsync *g = (struct gsync*)param;
		g->redraw();
	}

	static void filter_change_timer(void *param)
	{
		struct gsync *g = (struct gsync*)param;
		ffui_thd_post(filter_change, g);
	}

	void selected_file_exec(uint id)
	{
		int i = this->wmain.view.selected_first();
		if (i < 0)
			return;

		const struct ds_sync_diff_entry *de = this->entry_at(i);
		ffstr name = {};
		if (id == A_SRC_EXEC)
			name = de->lname;
		else if (id == A_DST_EXEC)
			name = de->rname;

		if (name.len) {
			char *zname = ffsz_dupstr(&name);
			ffui_exec(zname);
			ffmem_free(zname);
		}
	}

	void selected_file_dir_show(uint id)
	{
		int i = this->wmain.view.selected_first();
		if (i < 0)
			return;

		const struct ds_sync_diff_entry *de = this->entry_at(i);
		ffstr dir = {};
		if (id == A_SRC_SHOW_DIR && de->lname.len)
			ffpath_splitpath_str(de->lname, &dir, NULL);
		else if (id == A_DST_SHOW_DIR && de->rname.len)
			ffpath_splitpath_str(de->rname, &dir, NULL);

		if (dir.len) {
			char *zdir = ffsz_dupstr(&dir);
			ffui_openfolder1(zdir);
			ffmem_free(zdir);
		}
	}

	void selected_file_del(uint id)
	{
		xxvec names;
		this->sel_indices.acquire(this->wmain.view.selected());
		uint *i;
		FFSLICE_WALK(&this->sel_indices, i) {
			const struct ds_sync_diff_entry *de = this->entry_at(*i);
			const char *s = (id == A_SRC_DELETE) ? de->lname.ptr : de->rname.ptr;
			if (!s)
				continue;
			ffstr_dupz(names.push_z<ffstr>(), s);
		}

		ds_trash_conf c = {};
		c.input = names;
		names.reset();
		c.delete_on_fail = 1;
		c.on_complete = selected_files_deleted__worker;
		c.opaque = this;
		ds_trash_run(&c);
	}

	static void selected_files_deleted__worker(void *param, int result)
	{
		gsync *g = (gsync*)param;
		uint *i;
		FFSLICE_WALK(&g->sel_indices, i) {
			const struct ds_sync_diff_entry *de = g->entry_at(*i);
			g->sync_if->status(g->diff, de->id
				, DS_SYNC_SYNCING | DS_SYNC_ERROR | DS_SYNC_DONE, DS_SYNC_DONE);
		}

		ffui_thd_post(selected_files_deleted, g);
	}

	static void selected_files_deleted(void *param)
	{
		gsync *g = (gsync*)param;
		uint *i;
		FFSLICE_WALK(&g->sel_indices, i) {
			g->wmain.view.update(*i, 0);
		}

		g->wmain.stbar.text(0, xxstr_buf<100>().zfmt("Moved %L files to Trash", g->sel_indices.len));
		g->sel_indices.free();
	}

	void selected_filter(uint flags)
	{
		ffslice sel = this->wmain.view.selected();
		uint *i;
		FFSLICE_WALK(&sel, i) {
			const struct ds_sync_diff_entry *de = this->entry_at(*i);
			ffstr name;
			ffpath_splitpath_str((de->lname.len) ? de->lname : de->rname, NULL, &name);
			ffpath_splitname_str(name, &name, NULL);
			this->wmain.include.text(name);
			break;
		}
		ffslice_free(&sel);

		this->redraw();
	}

	void wnd_new()
	{
		const char *argv[] = {
			"dsynco-gui", NULL, NULL, NULL
		};
		const char *exe_name = "dsynco-gui";
#ifdef FF_WIN
		exe_name = "dsynco-gui.exe";
#endif
		xxvec exe(core->path(exe_name)), lpath = this->wmain.lpath.text(), rpath = this->wmain.rpath.text();
		argv[1] = lpath.strz();
		argv[2] = rpath.strz();
		ffps ps = ffps_exec(exe.strz(), argv, core->env);
		if (ps == FFPS_NULL) {
			ds_syserrlog("ps exec: %s", exe.strz());
			return;
		}

		ds_dbglog("spawned PID %u", ffps_id(ps));
		ffps_close(ps);
	}

	void sort(uint flags)
	{
		if (!this->diff) return;

		uint n = this->sync_if->sort(this->diff, flags);
		this->wmain.view.length(n, 1);
	}

	void core_task_add(ds_task_func func)
	{
		core->task(&this->core_task, func, this);
	}

	static void wmain_action(ffui_window *wnd, int id)
	{
		struct wmain *wmain = FF_STRUCTPTR(struct wmain, wnd, wnd);
		struct gsync *g = FF_STRUCTPTR(struct gsync, wmain, wmain);

		switch (id) {
		case A_WND_NEW:
			g->wnd_new(); break;

		case A_SCAN_CMP:
			g->scan_state = 0;
			g->core_task_add(scan_and_compare__worker);
			break;
		case A_SCAN_DUP:
			g->scan_state = 0;
			g->core_task_add(scan_dups__worker);
			break;

		case A_SYNC:
			g->sync_begin(g->sync_flags_add); break;
		case A_SYNC_DATE:
			g->sync_begin(DS_SYNC_SYNC_REPLACE_DATE); break;

		case A_SWAP:
			g->src_dst_swap(); break;

		case A_DIFF_NO_DATE:
			g->mfile.check(id, bit_toggle(&g->diff_flags, DS_SYNC_DIFF_NO_TIME));  break;

		case A_DIFF_MOVE_NO_NAME:
			g->mfile.check(id, bit_toggle(&g->diff_flags, DS_SYNC_DIFF_MOVE_NO_NAME));  break;

		case A_SYNC_FDEL:
			g->mfile.check(id, bit_toggle(&g->sync_flags_add, DS_SYNC_SYNC_FILE_DEL));  break;

		case A_SORT_FILESIZE:
			g->sort(DS_SYNC_SORT_FILESIZE); break;
		case A_SORT_FILEDATE:
			g->sort(DS_SYNC_SORT_MTIME); break;

		case A_INCLUDE_CHANGE:
		case A_EXCLUDE_CHANGE:
		case A_RECENTDAYS_CHANGE:
			core->timer(&g->timer, -500, filter_change_timer, g); break;

		case A_SHOW_LEFT:
			g->show_x(g->wmain.show_left, DS_SYNC_LEFT); break;
		case A_SHOW_EQ:
			g->show_x(g->wmain.show_eq, DS_SYNC_EQ); break;
		case A_SHOW_NEQ:
			g->show_x(g->wmain.show_neq, DS_SYNC_NEQ); break;
		case A_SHOW_MOV:
			g->show_x(g->wmain.show_mov, DS_SYNC_MOVE); break;
		case A_SHOW_RIGHT:
			g->show_x(g->wmain.show_right, DS_SYNC_RIGHT); break;
		case A_SHOW_DONE:
			g->show_x(g->wmain.show_done, DS_SYNC_DONE); break;
		case A_SHOW_NEQ_DATE:
			g->show_x(g->wmain.show_neq_date, DS_SYNC_NEWER | DS_SYNC_OLDER); break;
		case A_SHOW_INC_BOTH:
			g->props.include_both = !g->props.include_both;
			g->mlist.check(id, g->props.include_both);
			g->redraw();
			break;

		case A_SRC_EXEC:
		case A_DST_EXEC:
			if (ffui_ctl_focused(&g->wmain.view))
				g->selected_file_exec(id);
			break;

		case A_SRC_SHOW_DIR:
		case A_DST_SHOW_DIR:
			g->selected_file_dir_show(id); break;

		case A_SRC_DELETE:
		case A_DST_DELETE:
			if (ffui_ctl_focused(&g->wmain.view))
				g->selected_file_del(id);
			break;

		case A_FILTER_NAME:
			g->selected_filter(0);  break;

		case A_SEL_ALL:
			g->wmain.view.select_all(); break;

		case A_LIST_DISPLAY:
			g->list_cell_draw(g->wmain.view.dispinfo_item); break;

		case A_QUIT:
			g->wmain.wnd.close(); break;

		case A_ONCLOSE:
			g->quit(); break;
		}
	}

	static void* gui_ctl_find(void *udata, const ffstr *name)
	{
		#define _(m) FFUI_LDR_CTL(struct wmain, m)
		static const ffui_ldr_ctl wmain_ctls[] = {
			_(wnd),
			_(mmenu),
			_(llpath), _(lpath),
			_(lrpath), _(rpath),
			_(linclude), _(include),
			_(lexclude), _(exclude),
			_(ldays), _(recent_days),
			_(show_left), _(show_eq), _(show_neq), _(show_mov), _(show_right), _(show_done), _(show_neq_date),
			_(view),
			_(stbar),
			FFUI_LDR_CTL_END
		};
		#undef _

		static const ffui_ldr_ctl top_ctls[] = {
			FFUI_LDR_CTL(struct gsync, mfile),
			FFUI_LDR_CTL(struct gsync, mlist),
			FFUI_LDR_CTL(struct gsync, mpopup),
			FFUI_LDR_CTL3(struct gsync, wmain, wmain_ctls),
			FFUI_LDR_CTL_END
		};
		return ffui_ldr_findctl(top_ctls, udata, name);
	}

	static int gui_cmd_find(void *udata, const ffstr *name)
	{
		static const char action_str[][24] = {
			#define _(id)  #id
			INCLUDE_ACTIONS(_)
			#undef _
		};

		for (uint i = 0;  i != FF_COUNT(action_str);  i++) {
			if (ffstr_eqz(name, action_str[i]))
				return i + 1;
		}
		return 0;
	}

	int load_ui()
	{
		int r = -1;
		char *fn = core->path("dsynco.ui");
		if (!fn)
			return -1;

		ffui_loader ldr;
		ffui_ldr_init(&ldr, gui_ctl_find, gui_cmd_find, this);

		if (ffui_ldr_loadfile(&ldr, fn)) {
			ds_errlog("parsing dsynco.ui: %s", ffui_ldr_errstr(&ldr));
			goto done;
		}

		r = 0;

	done:
		ffui_ldr_fin(&ldr);
		ffmem_free(fn);
		return r;
	}

	static void wmain_show(void *param)
	{
		struct gsync *g = (struct gsync*)param;
		g->conf.status_update_period = 500;

		if (g->dir_src)
			g->wmain.lpath.text(g->dir_src);

		if (g->dir_dst)
			g->wmain.rpath.text(g->dir_dst);

		g->show_flags = DS_SYNC_LEFT
			| DS_SYNC_RIGHT
			| DS_SYNC_NEQ
			| DS_SYNC_MOVE
			// | DS_SYNC_EQ
			// | DS_SYNC_DONE
			| DS_SYNC_NEWER | DS_SYNC_OLDER
			;
		g->wmain.show_left.check(g->show_flags & DS_SYNC_LEFT);
		// g->wmain.show_eq.check(1);
		g->wmain.show_neq.check(g->show_flags & DS_SYNC_NEQ);
		g->wmain.show_mov.check(g->show_flags & DS_SYNC_MOVE);
		g->wmain.show_right.check(g->show_flags & DS_SYNC_RIGHT);
		// g->wmain.show_done.check(1);
		g->wmain.show_neq_date.check(g->show_flags & (DS_SYNC_NEWER | DS_SYNC_OLDER));

		g->wmain.wnd.show(1);
	}

	static int FFTHREAD_PROCCALL gui_thread(void *param)
	{
		struct gsync *g = (struct gsync*)param;
		ffui_init();
		if (g->load_ui())
			goto end;

		ffui_thd_post(wmain_show, g);

		ds_dbglog("entering GUI loop");
		ffui_run();
		ds_dbglog("exited GUI loop");

	end:
		ffui_uninit();
		core->exit(0);
		return 0;
	}

	void run()
	{
	#ifdef FF_WIN
		this->wmain.wnd.top = 1;
		this->wmain.wnd.manual_close = 1;
	#endif
		this->wmain.wnd.on_action = wmain_action;
		this->wmain.wnd.onclose_id = A_ONCLOSE;
		this->wmain.view.dispinfo_id = A_LIST_DISPLAY;

		if (FFTHREAD_NULL == (this->thread = ffthread_create(gui_thread, this, 0)))
			DS_ASSERT(0);
	}
};

FF_EXTERN struct gsync* gsync_create(const ds_core *_core, const char *dir_src, const char *dir_dst)
{
	core = _core;
	struct gsync *s = new(ffmem_new(struct gsync)) struct gsync;
	s->sync_if = core->sync_if;
	s->dir_src = dir_src;
	s->dir_dst = dir_dst;
	return s;
}

FF_EXTERN void gsync_close(struct gsync *s)
{
	if (!s) return;

	s->~gsync();
	ffmem_free(s);
}

FF_EXTERN void gsync_run(struct gsync *s)
{
	s->run();
}
