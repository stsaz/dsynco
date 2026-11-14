/** dsynco: copy: write output file
2022, Simon Zolin */

static int output_init(struct copy *c)
{
	struct ds_file_conf fc = {};
	fc.buffer_size = c->conf.buffer_size;
	fc.n_buffers = 1;
	c->o.f = core->file->create(&fc);
	return 0;
}

static void output_close(struct copy *c)
{
	core->file->destroy(c->o.f);
	if (c->o.del_on_close) {
		if (c->conf.atomic) {
			core->file->del(c->o.name_tmp, 0);
		}
		core->file->del(c->o.name, 0);
	}
	ffmem_free0(c->o.name_tmp);
}

static void output_trash_complete(void *param, int result)
{
	struct copy *c = (struct copy*)param;
	copy_run(c);
}

static int output_fin(struct copy *c)
{
	switch (c->o.state) {
	case 0:
		core->file->attr_set(c->o.f, fffileinfo_attr(&c->fi));
		core->file->close(c->o.f);
		c->o.del_on_close = 0;

		if (c->conf.atomic && 0 != fffileinfo_size(&c->o.fi)) {

			struct ds_trash_conf tc = {};

			ffstr *p = ffvec_pushT(&tc.input, ffstr);
			p->ptr = ffsz_dup(c->o.name);
			p->len = ffsz_len(p->ptr);

			tc.test = c->conf.test;
			tc.on_complete = output_trash_complete;
			tc.opaque = c;
			ds_dbglog("copy: trash: %s", c->o.name);
			c->o.state = 1;
			ds_trash_run(&tc);
			return 'asyn';
		}
		// fallthrough

	case 1:
		if (c->conf.atomic && !c->conf.test
			&& 0 != fffile_rename(c->o.name_tmp, c->o.name)) {
			ds_syserrlog("fffile_rename: %s -> %s", c->o.name_tmp, c->o.name);
			return 0xbad;
		}
		break;
	}

	return 0;
}

static void output_reset(struct copy *c)
{
	c->o.state = 0;
	c->o.total = 0;
	c->o.off = 0;
	ffmem_free0(c->o.name_tmp);
}

static int output_open(struct copy *c)
{
	ffmem_free0(c->o.name_tmp);
	const struct ds_copy_io *cio = ffslice_itemT(&c->conf.in_out, c->nfiles - 1, struct ds_copy_io);
	c->o.name = cio->out;
	if (c->conf.atomic)
		c->o.name_tmp = ffsz_allocfmt("%s.ds_tmp", c->o.name);

	if (fffile_isdir(fffileinfo_attr(&c->fi))) {
		int r = core->file->dir_create(c->o.name, DS_FILE_DIR_RECURSIVE);
		if (r == DS_FILE_ERR) return 0xbad;
		return 'skip';
	}

	// Copy symlink
#ifdef FF_UNIX
	if ((fffileinfo_attr(&c->fi) & FFFILEATTR_UNIX_TYPEMASK) == FFFILEATTR_UNIX_LINK) {
		char target[4096];
		int r = readlink(c->iname, target, sizeof(target) - 1);
		if (r < 0 || r == sizeof(target) - 1) {
			if (r < 0)
				ds_syserrlog("readlink: %s", c->iname);
			else
				ds_errlog("readlink truncation: %s", c->iname);
			return 0xbad;
		}
		target[r] = '\0';

		uint f = 0;
		if (c->conf.overwrite)
			f |= DS_FILE_CREATE_TRUNC;
		else
			f |= DS_FILE_CREATENEW;
		if (c->conf.test)
			f |= DS_FILE_FAKEWRITE;
		if (c->conf.directio)
			f |= DS_FILE_DIRECTIO;

		if (DS_FILE_ERR == core->file->link(target, c->o.name, f))
			return 0xbad;
		return 'skip';
	}
#endif

	if (c->conf.update
		&& !fffile_info_path(c->o.name, &c->o.fi)) {

		if (fffile_isdir(fffileinfo_attr(&c->o.fi))) {
			ds_errlog("%s: target path is an existing directory", c->o.name);
			return 0xbad;
		}

		if (c->conf.replace_date) {
			fftime t = fffileinfo_mtime(&c->fi);
			if (fffile_set_mtime_path(c->o.name, &t)) {
				ds_syserrlog("fffile_set_mtime_path: %s", c->o.name);
				return 0xbad;
			}
			ds_verblog("replace date: %s", c->o.name);
			return 'skip';
		}

		if (fftime_cmp_val(fffileinfo_mtime1(&c->fi), fffileinfo_mtime1(&c->o.fi)) <= 0) {
			ds_dbglog("update: target file is of the same date or newer; skipping");
			return 'skip';
		}
	}

	uint flags = DS_FILE_WRITE | DS_FILE_NO_PREALLOC;
	if (c->conf.overwrite)
		flags |= DS_FILE_CREATE_TRUNC;
	else
		flags |= DS_FILE_CREATENEW;
	if (c->conf.test)
		flags |= DS_FILE_FAKEWRITE;
	if (c->conf.directio)
		flags |= DS_FILE_DIRECTIO;

	int r = core->file->open(c->o.f, c->o.name, flags);
	if (r == DS_FILE_ERR)
		return 0xbad;

	r = core->file->info(c->o.f, &c->o.fi);
	if (r == DS_FILE_ERR)
		return 0xbad;

	if (c->conf.atomic) {
		core->file->close(c->o.f);
		r = core->file->open(c->o.f, c->o.name_tmp, flags);
		if (r == DS_FILE_ERR)
			return 0xbad;

		fffileinfo fi;
		r = core->file->info(c->o.f, &fi);
		if (r == DS_FILE_ERR)
			return 0xbad;
		if (0 != fffileinfo_size(&fi)) {
			ds_errlog("temp file %s already exists - check & delete it manually", c->o.name_tmp);
			return 0xbad;
		}
	}

	if (DS_FILE_ERR == core->file->trunc(c->o.f, fffileinfo_size(&c->fi)))
		return 0xbad;

	c->o.del_on_close = !c->conf.test
		&& (c->conf.atomic || 0 == fffileinfo_size(&c->o.fi));
	return 0;
}

static int output_write(struct copy *c, ffstr input)
{
	int r = core->file->write(c->o.f, input, c->o.off);
	if (r == DS_FILE_ERR) return 0xbad;
	c->o.off += input.len;
	c->o.total += input.len;
	return 0;
}
