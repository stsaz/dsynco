/** dsynco: sync: scan file tree
2022, Simon Zolin */

#include <util/util.h>

struct ent {
	char type; // 'f', 'd'
	ffstr name;
	struct ds_sync_entry d;
};

struct snapshot {
	ffstr		root_dir;
	fntree_block *root, *parent_blk;
	fntree_cursor cur;
	ffvec		name;
	ffstr		path, name_segment;
	uint64		total;
};

void snapshot_close(struct snapshot *s)
{
	fntree_free_all(s->root);
	ffstr_free(&s->root_dir);
	ffvec_free(&s->name);
}

/** Prepare full file name */
static void sync_full_name(ffvec *buf, const fntree_entry *e, const fntree_block *b)
{
	buf->len = 0;
	if (e == NULL)
		return;
	ffstr name = fntree_name(e);
	ffstr path = fntree_path(b);
	if (path.len != 0)
		ffvec_addfmt(buf, "%S%c", &path, FFPATH_SLASH);
	ffvec_addfmt(buf, "%S%Z", &name);
	buf->len--;
}

/** Get next file from tree */
int snapshot_next(struct snapshot *s, ffstr *pname, fntree_entry **e)
{
	fntree_block *b = s->root;
	fntree_entry *it = fntree_cur_next_r_ctx(&s->cur, &b);
	if (!it) {
		ds_dbglog("no more input files");
		return 'done';
	}

	ffstr nm = fntree_name(it);
	s->path = fntree_path(b);
	s->name.len = 0;
	if (s->path.len)
		ffvec_addfmt(&s->name, "%S%c", &s->path, FFPATH_SLASH);
	ffvec_addfmt(&s->name, "%S%Z", &nm);
	s->name_segment = nm;

	ffstr_set(pname, s->name.ptr, s->name.len - 1);
	ds_dbglog("file: '%S'", pname);
	*e = it;

	if (s->parent_blk != b) {
		s->parent_blk = b;
		return 'nblk';
	}
	return 0;
}

/** Add tree branch */
int snapshot_add_dir(struct snapshot *s, fffd fd)
{
	ffdirscan ds = {};
	uint flags = 0;

	if (fd != FFFILE_NULL) {
#ifdef FF_LINUX
		ds.fd = fd;
		flags = FFDIRSCAN_USEFD;
#else
		fffile_close(fd);
#endif
	}

	if (ffdirscan_open(&ds, (char*)s->name.ptr, flags)) {
		ds_syserrlog("ffdirscan_open: %s", s->name.ptr);
		return -1;
	}
	ffstr path = FFSTR_INITZ((char*)s->name.ptr);
	fntree_block *b = fntree_from_dirscan(path, &ds, sizeof(struct ds_sync_entry));
	s->total += b->entries;
	fntree_attach((fntree_entry*)s->cur.cur, b);
	ds_dbglog("added branch '%S' with %u files [%U]", &path, b->entries, s->total);
	ffdirscan_close(&ds);
	return 0;
}

/** Get file attributes */
static int sync_f_info(struct ds_sync_entry *d, const fffileinfo *fi)
{
	int rc = 'f';
	if (fffile_isdir(fffileinfo_attr(fi)))
		rc = 'd';

	d->size = fffileinfo_size(fi);
	d->mtime = fffileinfo_mtime1(fi);
	d->mtime.nsec = (d->mtime.nsec / 1000000) * 1000000;

#ifdef FF_UNIX
	d->unix_attr = fffileinfo_attr(fi);
	d->win_attr = (fffile_isdir(fffileinfo_attr(fi))) ? FFFILE_WIN_DIR : 0;
	d->uid = fi->st_uid;
	d->gid = fi->st_gid;
#else
	d->win_attr = fffileinfo_attr(fi);
	d->unix_attr = (fffile_isdir(fffileinfo_attr(fi))) ? FFFILE_UNIX_DIR : 0;
#endif

	// d->crc32 = ;
	return rc;
}

int snapshot_scan_next(struct snapshot *s, struct ent *dst)
{
	fntree_entry *e;
	ffstr name;
	int r = snapshot_next(s, &name, &e);
	if (r == 'done')
		return 'done';

	struct ds_sync_entry *d = (struct ds_sync_entry*)fntree_data(e);
	ffmem_zero_obj(d);

	int ftype = 'f';
	fffd fd = FFFILE_NULL;
	fffileinfo fi;
	if (fffile_info_path(name.ptr, &fi)) {
		ds_syswarnlog("fffile_info_path: %s", name.ptr);
		goto fin;
	}

	ftype = sync_f_info(d, &fi);
	switch (ftype) {
	case 'd':
		if (snapshot_add_dir(s, fd)) {
			fd = FFFILE_NULL;
			r = 'erro';
			goto end;
		}
		fd = FFFILE_NULL;
		break;

	case 'f':
		break;
	}

fin:
	if (dst) {
		dst->type = ftype;
		dst->name = name;
		dst->d = *d;
	}

end:
	fffile_close(fd);
	return r;
}

static void sync_snapshot_free(ds_sync_snapshot *ss)
{
	if (ss)
		snapshot_close(ss);
	ffmem_free(ss);
}

// ""->["/"]
static ds_sync_snapshot* sync_scan(ffstr path, uint flags)
{
	ds_sync_snapshot *ss = ffmem_new(ds_sync_snapshot);
	ss->root = fntree_create(FFSTR_Z(""));
	if (!fntree_add(&ss->root, path, sizeof(struct ds_sync_entry)))
		goto end;

	ffstr_dupstr(&ss->root_dir, &path);

	if (!(flags & DS_SYNC_SCAN_STEP)) {
		for (;;) {
			int r = snapshot_scan_next(ss, NULL);
			if (r == 'done')
				break;
			else if (r == 'erro')
				continue;
		}
	}

	return ss;

end:
	sync_snapshot_free(ss);
	return NULL;
}

static int sync_scan_next(ds_sync_snapshot *ss)
{
	int r = snapshot_scan_next(ss, NULL);
	if (r == 'done')
		return 1;
	else if (r == 'erro')
		return -1;
	return 0;
}

static void fntree_entry_set(fntree_entry *dst, const fntree_entry *src, size_t data_len)
{
	fntree_attach(dst, src->children);
	ffmem_copy(fntree_data(dst), fntree_data(src), data_len);
}

/** Combine two directories.
Example:
	1. ""->["/path/a"]      | "/path/b" => ""->["/path"->[a,b]]
	2. ""->["/path"->[a,b]] | "/path/c" => ""->["/path"->[a,b,c]]
*/
static void sync_combine(struct snapshot *s, fntree_entry *e2)
{
	ffstr p1 = s->root_dir, p2 = fntree_path(e2->children), parent, name1, name2;
	ds_dbglog("sync: combine: %S + %S", &p1, &p2);
	if (ffpath_parent(p1, p2, &parent))
		return;
	ffpath_splitpath_str(p2, NULL, &name2);

	const uint data_len = sizeof(struct ds_sync_entry);
	if (!ffstr_eq2(&parent, &p1)) {
		// 1.
		fntree_block *first = fntree_create(parent);

		ffpath_splitpath_str(p1, NULL, &name1);
		fntree_entry *e = fntree_add(&first, name1, data_len);
		fntree_entry_set(e, _fntr_ent_first(s->root), data_len);

		e = fntree_add(&first, name2, data_len);
		fntree_entry_set(e, e2, data_len);

		fntree_block *newroot = fntree_create(FFSTR_Z(""));
		e = fntree_add(&newroot, parent, data_len);
		fntree_attach(e, first);

		s->root = newroot;
		s->root_dir.len = parent.len;
		s->total += 3;

	} else {
		// 2.
		fntree_block **root = &_fntr_ent_first(s->root)->children;
		fntree_entry *e = fntree_add(root, name2, data_len);
		fntree_entry_set(e, e2, data_len);
		s->total++;
	}

	e2->children = NULL;
}

static struct snapshot* sync_scan_wc(ffstr path, uint flags)
{
	ffdirscan ds = {};
	const char *fn;
	char *fullname = NULL, *dirz = NULL;
	struct snapshot *r = NULL, *d = NULL, *first = NULL;
	ffvec dirs = {}; // char*[]
	char **it;
	uint idir = 0;

	ffstr dir, name;
	ffpath_splitpath_str(path, &dir, &name);
	dirz = ffsz_dupstr(&dir);
	ds.wildcard = name.ptr;
	if (ffdirscan_open(&ds, dirz, FFDIRSCAN_USEWILDCARD)) {
		ds_syserrlog("ffdirscan_open: '%s'  wc: '%s'", dirz, name.ptr);
		goto end;
	}

	while ((fn = ffdirscan_next(&ds))) {

		ffmem_free(fullname);
		fullname = ffsz_allocfmt("%s%c%s", dirz, FFPATH_SLASH, fn);

		fffileinfo fi;
		if (!fffile_info_path(fullname, &fi)
			&& fffile_isdir(fffileinfo_attr(&fi))) {
			*ffvec_pushT(&dirs, char*) = fullname;
			fullname = NULL;
		}
	}

	while (idir < dirs.len) {
		const char *namez = *ffslice_itemT(&dirs, idir++, char*);
		if (!(d = sync_scan(FFSTR_Z(namez), flags | DS_SYNC_SCAN_STEP)))
			goto end;
		if (!first)
			first = d;

		for (;;) {
			int r = snapshot_scan_next(d, NULL);
			if (r == 'done')
				break;
			else if (r == 'erro')
				continue;
		}

		if (d != first) {
			sync_combine(first, _fntr_ent_first(d->root));
			first->total += d->total;
		}
		d = NULL;
	}

	r = first;
	first = NULL;

end:
	FFSLICE_WALK(&dirs, it) {
		ffmem_free(*it);
	}
	ffvec_free(&dirs);
	ffmem_free(dirz);
	ffmem_free(fullname);
	ffdirscan_close(&ds);
	sync_snapshot_free(d);
	sync_snapshot_free(first);
	return r;
}
