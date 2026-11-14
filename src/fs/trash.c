/** dsynco: move files to user's trash directory
2022, Simon Zolin */

#ifdef _WIN32
#include <util/windows-shell.h>
#endif
#include <dsynco.h>
#include <util/unix-shell.h>
#include <ffsys/path.h>

extern const ds_core *core;

struct trash {
	struct ds_trash_conf conf;
	ds_task task;

	ffvec names; //char*[]
	uint n_trashed, n_deleted;
};

static void trash_free(struct trash *t)
{
	char **p;
	FFSLICE_WALK(&t->names, p) {
		ffmem_free(*p);
	}
	ffvec_free(&t->names);
	ffvec_free(&t->conf.input);

	ffmem_free(t);
}

/** Move files to Trash */
static int f_trash(struct trash *t, const char **names, ffsize names_n)
{
#ifdef FF_LINUX
	int e = 0;
	for (ffsize i = 0;  i != names_n;  i++) {
		const char *err;
		if (0 != ffui_glib_trash(names[i], &err)) {
			if (!t->conf.delete_on_fail) {
				ds_warnlog("%s: can't move file to trash: %s"
					, names[i], err);
			}
			e = 1;
		}
	}
	if (e)
		return -1;

#else
	if (0 != ffui_file_del(names, names_n, FFUI_FILE_TRASH)) {
		if (!t->conf.delete_on_fail)
			ds_syswarnlog("can't move files to trash");
		return -1;
	}
#endif

	return 0;
}

/** Delete files from disk
Return N of files deleted */
static int f_del(ffvec names)
{
	uint n = 0;
	const char **namez;
	FFSLICE_WALK(&names, namez) {
		if (0 != core->file->del(*namez, 0))
			n++;
	}

	if (n != 0) {
		ds_errlog("%u files were not deleted", n);
	}

	return names.len - n;
}

static int trash_all(struct trash *t)
{
	if (t->conf.test) {
		t->n_trashed += t->names.len;
		return 0;
	}

	int r;
	if ((r = f_trash(t, t->names.ptr, t->names.len))) {
		if (t->conf.delete_on_fail) {
			ds_verblog("can't move files to trash: error code %d.  Trying to delete them.", r);
			if (0 == (t->n_deleted = f_del(t->names)))
				return -1;

		} else {
			return -1;
		}
	} else {
		t->n_trashed += t->names.len;
	}

	if (core->verbose) {
		const char **namez;
		FFSLICE_WALK(&t->names, namez) {
			ds_verblog("trash: '%s'", *namez);
		}
	}
	return 0;
}

static void trash_done(struct trash *t)
{
	if (core->verbose) {
		ds_verblog("%u files were moved to Trash", t->n_trashed);
		if (t->n_deleted != 0) {
			ds_verblog("%u files were deleted", t->n_deleted);
		}
	}

	uint n = t->names.len - (t->n_trashed + t->n_deleted);
	if (n != 0) {
		ds_errlog("%u files were skipped", n);
	}
}

static void trash_run(void *param)
{
	struct trash *t = param;
	int rc = 1;
	ffstr *it;
	FFSLICE_WALK(&t->conf.input, it) {
		char **p = ffvec_pushT(&t->names, char*);
		*p = ffsz_dupstr(it);
	}
	if (trash_all(t))
		goto end;
	trash_done(t);
	rc = 0;

end:
	{
	void (*on_complete)(void*, int) = t->conf.on_complete;
	void *udata = t->conf.opaque;
	trash_free(t);
	ds_dbglog("trash: calling on_complete(): %d", rc);
	on_complete(udata, rc);
	}
}

int ds_trash_run(const struct ds_trash_conf *conf)
{
	struct trash *t = ffmem_new(struct trash);
	t->conf = *conf;
	core->task(&t->task, trash_run, t);
	return 0;
}
