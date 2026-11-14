/** dsynco: core: ds_file_if implementation
2022, Simon Zolin */

#include <dsynco.h>
#include <util/fbuf.h>
#include <ffsys/std.h>
#include <ffsys/dir.h>
#include <ffsys/perf.h>

extern ds_core *core;
#define ALIGN (4*1024)

struct file {
	char *name;
	fffd fd;
	uint open_flags;
	uint buffer_size;
	fftime mtime;

	struct fbufset bufset;
	uint64 size;
	uint64 cur_off;

	uint w_write :1;

	struct {
		struct fbuf buf;
		ffstr user_data;
		uint continued :1;
	} wcache;

	struct {
		ffstr buf;
		uint64 off;
		uint64 prealloc;
		uint async :1;
	} w;

	struct {
		fftime t_read, t_write;
		uint64 total_read, total_write;
	} stats;
};

static int f_write(struct file *f, ffstr d, uint64 off);
static int file_flush(ds_file_obj *_f);
static int file_trunc(ds_file_obj *_f, int64 size);

static ds_file_obj* file_create(struct ds_file_conf *conf)
{
	struct file *f = ffmem_new(struct file);
	f->fd = FFFILE_NULL;
	f->buffer_size = conf->buffer_size;
	if (f->buffer_size == 0)
		f->buffer_size = 64*1024;
	if (conf->n_buffers == 0)
		conf->n_buffers = 3;

	f->buffer_size = ffmax(f->buffer_size, ALIGN);
	fbufset_init(&f->bufset, conf->n_buffers, f->buffer_size, ALIGN);
	f->wcache.buf.ptr = ffmem_align(f->buffer_size, ALIGN);
	return f;
}

static void mtime_set(struct file *f)
{
	if (f->mtime.sec == 0)
		return;

	fftime time1970 = f->mtime;
	time1970.sec -= FFTIME_1970_SECONDS;
	if (0 != fffile_set_mtime(f->fd, &time1970)) {
		ds_syswarnlog("file set mtime: %s", f->name);
	} else if (core->debug) {
		char buf[128];
		ffdatetime dt;
		fftime_split1(&dt, &f->mtime);
		int r = fftime_tostr1(&dt, buf, sizeof(buf), FFTIME_YMD);
		buf[r] = '\0';
		ds_dbglog("%s: mtime: %s", f->name, buf);
	}
}

static void prealloc_trunc(struct file *f)
{
	if (f->w.prealloc <= f->size) return;

	ds_dbglog("%s: truncate: %U/%U", f->name, f->size, f->w.prealloc);

	fffd fd = f->fd;

#ifdef FF_WIN
	if ((f->open_flags & DS_FILE_DIRECTIO)
		&& (f->size % ALIGN) != 0) {
		if (FFFILE_NULL == (fd = fffile_open(f->name, FFFILE_WRITEONLY))) {
			ds_syserrlog("%s: fffile_open", f->name);
			return;
		}
		ds_dbglog("%s: opened without FFFILE_DIRECT", f->name);
	}
#endif

	if (0 != fffile_trunc(fd, f->size)) {
		ds_syserrlog("%s: fffile_trunc", f->name);
		goto end;
	}
	f->w.prealloc = f->size;

end:
	if (fd != f->fd)
		fffile_close(fd);
}

static void file_close(ds_file_obj *_f)
{
	struct file *f = _f;
	if (f->fd != FFFILE_NULL) {

		if (f->open_flags & (DS_FILE_WRITE | DS_FILE_READWRITE)) {
			if (DS_FILE_ASYNC == file_flush(f))
				ds_syserrlog("file_flush");
			prealloc_trunc(f);
			mtime_set(f);
		}

		if (0 != fffile_close(f->fd)) {
			ds_syserrlog("%s: fffile_close", f->name);
		} else {
			if (f->open_flags & (DS_FILE_WRITE | DS_FILE_READWRITE)) {
				ds_dbglog("saved file: %s (%,U)  %UMB/sec | %UMB/sec"
					, f->name, f->size
					, FFINT_DIVSAFE(f->stats.total_read, fftime_to_usec(&f->stats.t_read))
					, FFINT_DIVSAFE(f->stats.total_write, fftime_to_usec(&f->stats.t_write))
					);
			} else {
				ds_dbglog("read file: %s  %UMB/sec | %UMB/sec"
					, f->name
					, FFINT_DIVSAFE(f->stats.total_read, fftime_to_usec(&f->stats.t_read))
					, FFINT_DIVSAFE(f->stats.total_write, fftime_to_usec(&f->stats.t_write))
					);
			}
		}

		f->fd = FFFILE_NULL;
	}

	ffmem_free(f->name); f->name = NULL;
}

static void file_destroy(ds_file_obj *_f)
{
	struct file *f = _f;
	if (f == NULL)
		return;

	file_close(f);
	ffmem_alignfree(f->wcache.buf.ptr);
	fbufset_destroy(&f->bufset);
	ffmem_free(f->name);
	ffmem_free(f);
}

static int file_open(ds_file_obj *_f, const char *name, uint how)
{
	struct file *f = _f;
	file_close(f);
	f->open_flags = how;

	fbufset_reset(&f->bufset);
	f->size = 0;
	f->w.prealloc = 0;
	f->cur_off = 0;
	ffmem_zero_obj(&f->mtime);

	f->name = ffsz_dup(name);

	if (how & DS_FILE_FAKEWRITE)
		return DS_FILE_OK;

	uint flags = FFFILE_READONLY;
	if ((how & 3) == DS_FILE_WRITE)
		flags = FFFILE_WRITEONLY;
	else if ((how & 3) == DS_FILE_READWRITE)
		flags = FFFILE_READWRITE;

	if (how & DS_FILE_CREATE)
		flags |= FFFILE_CREATE;
	else if (how & DS_FILE_CREATENEW)
		flags |= FFFILE_CREATENEW;

	if (how & DS_FILE_DIRECTIO)
		flags |= FFFILE_DIRECT;

#ifdef FF_WIN
	if (how & DS_FILE_READAHEAD)
		flags |= FILE_FLAG_SEQUENTIAL_SCAN;
#endif

	flags |= FFFILE_NOATIME;

	for (uint i = 0;  ;  i++) {
		if (FFFILE_NULL == (f->fd = fffile_open(f->name, flags))) {

#ifdef FF_UNIX
			if (fferr_last() == EINVAL && (flags & FFFILE_DIRECT)) {
				ds_dbglog("directio: EINVAL");
				flags &= ~FFFILE_DIRECT;
				if (how & DS_FILE_CREATENEW) {
					flags &= ~FFFILE_CREATENEW;
					flags |= FFFILE_CREATE;
				}
				continue;
			}
#endif

			if (i == 0 && fferr_notexist(fferr_last())
				&& (how & (DS_FILE_WRITE | DS_FILE_READWRITE))) {
				if (0 != ffdir_make_path(f->name, 0)) {
					ds_syserrlog("ffdir_make_path: for filename %s", f->name);
					return DS_FILE_ERR;
				}
				continue;
			}

			ds_syserrlog("file open: %s", f->name);
			return DS_FILE_ERR;
		}
		break;
	}

	if ((how & DS_FILE_DIRECTIO) && !(flags & FFFILE_DIRECT)) {
		ds_infolog("%s: opened without DIRECT_IO flag", f->name);
		f->open_flags &= ~DS_FILE_DIRECTIO;
	}

	if ((how & DS_FILE_CREATE_TRUNC) == DS_FILE_CREATE_TRUNC) {
		file_trunc(f, 0);
	}

	ds_dbglog("%s: opened file", f->name);
	return DS_FILE_OK;
}

static int f_benchmark(fftime *t)
{
	if (core->verbose) {
		*t = fftime_monotonic();
		return 1;
	}
	return 0;
}

static int file_read(ds_file_obj *_f, ffstr *d, int64 off)
{
	struct file *f = _f;
	struct fbuf *b;

	if (off == -1)
		off = f->cur_off;

	if (NULL != (b = fbufset_find(&f->bufset, off))) {
		ds_dbglog("%s: @%U: cache hit: %L @%U", f->name, off, b->len, b->off);
		goto done;
	}

	b = fbufset_nextbuf(&f->bufset);

	fftime t1, t2 = {};
	f_benchmark(&t1);

	ffuint64 offset = off;
	ffssize r;
	offset = ffint_align_floor2(off, ALIGN);
	r = fffile_readat(f->fd, b->ptr, f->buffer_size, offset);

	if (r < 0 && fferr_again(fferr_last()))
		return DS_FILE_ASYNC;

	if (r < 0) {
		ds_syserrlog("file read: %s", f->name);
		return DS_FILE_ERR;
	}

	if (f_benchmark(&t2)) {
		fftime_sub(&t2, &t1);
		fftime_add(&f->stats.t_read, &t2);
		f->stats.total_read += b->len;
	}

	if (r < f->buffer_size)
		f->size = offset + r;
	b->off = offset;
	b->len = r;
	ds_dbglog("%s: read %L @%U", f->name, b->len, b->off);

done:
	ffstr_set(d, b->ptr, b->len);
	ffstr_shift(d, off - b->off);
	f->cur_off = b->off + b->len;
	if (d->len == 0)
		return DS_FILE_EOF;
	return DS_FILE_OK;
}

/** Pass data to kernel */
static int f_write(struct file *f, ffstr d, uint64 off)
{
	ffsize len = d.len;
	ffssize r;

	fftime t1, t2 = {};
	f_benchmark(&t1);

	if (f->open_flags & DS_FILE_DIRECTIO) {
		len = ffint_align_ceil2(d.len, ALIGN);
		ffmem_fill(d.ptr + d.len, 0x00, len - d.len); // zero the trailer
	}
	r = fffile_writeat(f->fd, d.ptr, len, off);
	if (r > (ssize_t)d.len)
		r = d.len;

	if (r < 0 && fferr_again(fferr_last()))
		return -DS_FILE_ASYNC;

	if (r < 0) {
		ds_syserrlog("file write: %s %L @%U", f->name, len, off);
		return -DS_FILE_ERR;
	}

	if (f_benchmark(&t2)) {
		fftime_sub(&t2, &t1);
		fftime_add(&f->stats.t_write, &t2);
		f->stats.total_write += r;
	}

	ds_dbglog("%s: written %L @%U", f->name, r, off);
	if (off + r > f->size)
		f->size = off + r;
	if (f->w.prealloc < off + len)
		f->w.prealloc = off + len;
	return r;
}

static int file_flush(ds_file_obj *_f)
{
	struct file *f = _f;
	ffstr d = FFSTR_INITN(f->wcache.buf.ptr, f->wcache.buf.len);
	uint64 off = f->wcache.buf.off;
	while (d.len) {
		ffssize r = f_write(f, d, off);
		if (r < 0)
			return -r;
		off += r;
		ffstr_shift(&d, r);
	}
	f->wcache.buf.len = 0;
	f->wcache.buf.off = 0;
	return 0;
}

static int fw_write(struct file *f, ffstr *io_data, int64 *io_off)
{
	ffstr d = *io_data;
	int64 off = *io_off;
	if (f->w.async) {
		f->w.async = 0;
		d = f->w.buf;
		f->w.buf.len = 0;
		off = f->w.off;
	} else {
		FF_ASSERT(off != -1);
	}

	if (!(f->open_flags & DS_FILE_NO_PREALLOC) && f->w.prealloc < off + d.len) {
		f->w.prealloc = ffint_align_power2(off + d.len);
		if (fffile_trunc(f->fd, f->w.prealloc)) {
			ds_syswarnlog("fffile_trunc: %s", f->name);
			f->open_flags |= DS_FILE_NO_PREALLOC;
		}
	}

	ssize_t r = f_write(f, d, off);
	if (r == -DS_FILE_ASYNC) {
		f->w.buf = d;
		f->w.async = 1;
		f->w.off = off;
		return DS_FILE_ASYNC; // kernel buffer is full
	} else if (r < 0) {
		return -r; // failed to write data
	}

	f->w.off = off + r;
	if ((size_t)r != d.len) {
		ffstr_shift(&d, r);
		f->w.buf = d;
		f->w.async = 1;
		return DS_FILE_ASYNC; // kernel buffer is full
	}

	io_data->len = 0;
	return -1; // data chunk was written successfully
}

static int file_write(ds_file_obj *_f, ffstr data, int64 off)
{
	struct file *f = _f;
	if (f->open_flags & DS_FILE_FAKEWRITE)
		return DS_FILE_OK;

	if (off == -1)
		off = f->cur_off;
	f->cur_off = off + data.len;
	int r = fw_write(f, &data, &off);
	if (r >= 0)
		return r;
	return DS_FILE_OK;
}

static int file_write_fmt(ds_file_obj *_f, const char *fmt, ...)
{
	ffstr s = {};
	ffsize cap = 0;
	va_list va;
	va_start(va, fmt);
	ffstr_growfmtv(&s, &cap, fmt, va);
	va_end(va);

	int r = file_write(_f, s, -1);
	ffstr_free(&s);
	return r;
}

static int file_trunc(ds_file_obj *_f, int64 size)
{
	struct file *f = _f;

	if (f->open_flags & DS_FILE_FAKEWRITE)
		return 0;

	if (size == -1)
		size = f->cur_off;
	f->w.prealloc = size;

	if (0 != fffile_trunc(f->fd, f->w.prealloc)) {
		ds_syswarnlog("fffile_trunc: %s", f->name);
		f->open_flags |= DS_FILE_NO_PREALLOC;
	}
	ds_dbglog("%s: truncate: %U", f->name, f->w.prealloc);
	return 0;
}

static int file_behaviour(ds_file_obj *_f, uint flags)
{
	struct file *f = _f;
	switch (flags) {
	case DS_FBEH_SEQ:
		ds_dbglog("%s: sequential access", f->name);
		f->size = fffile_size(f->fd);
		if (0 != fffile_readahead(f->fd, f->size))
			ds_dbglog("file read ahead: %E", fferr_last());
		break;

	case DS_FBEH_RANDOM:
		ds_dbglog("%s: random access", f->name);
		if (0 != fffile_readahead(f->fd, -1))
			ds_dbglog("file read ahead: %E", fferr_last());
		break;

	case DS_FBEH_TRUNC_PREALLOC:
		if (DS_FILE_ASYNC == file_flush(f))
			ds_syserrlog("file_flush");
		prealloc_trunc(f);
		break;
	}
	return 0;
}

static fffd file_fd(ds_file_obj *_f, uint flags)
{
	struct file *f = _f;
	fffd fd = f->fd;
	if (flags & DS_FILE_ACQUIRE)
		f->fd = FFFILE_NULL;
	return fd;
}

static int file_info(ds_file_obj *_f, fffileinfo *fi)
{
	struct file *f = _f;

	if (f->open_flags & DS_FILE_FAKEWRITE) {
		ffmem_zero_obj(fi);
		return 0;
	}

	if (f->open_flags & DS_FILE_INFO_NOFOLLOW) {
		if (0 != fffile_info_linkpath(f->name, fi)) {
			ds_syserrlog("file get info: %s", f->name);
			return DS_FILE_ERR;
		}
		return 0;
	}

	if (0 != fffile_info(f->fd, fi)) {
		ds_syserrlog("file get info: %s", f->name);
		return DS_FILE_ERR;
	}
	return 0;
}

static int file_mtime_set(ds_file_obj *_f, fftime mtime)
{
	struct file *f = _f;
	if (f->open_flags & DS_FILE_FAKEWRITE)
		return DS_FILE_OK;

	f->mtime = mtime;
	return 0;
}

static int file_attr_set(ds_file_obj *_f, uint attr)
{
	struct file *f = _f;
	if (f->open_flags & DS_FILE_FAKEWRITE)
		return DS_FILE_OK;

	if (0 != fffile_set_attr(f->fd, attr)) {
		ds_syserrlog("fffile_set_attr: %s", f->name);
		return DS_FILE_ERR;
	}

	ds_dbglog("%s: attr: %xu", f->name, attr);
	return 0;
}

static int dir_create(const char *name, uint flags)
{
	int r = ffdir_make(name);
	if (r) {
		if (fferr_exist(fferr_last())) {
			ds_dbglog("%s: directory already exists", name);
			return 0;
		}

		if (flags & DS_FILE_DIR_RECURSIVE) {
			char *sz = ffsz_dup(name);
			r = ffdir_make_all(sz, 0);
			ffmem_free(sz);
		}

		if (r) {
			ds_syserrlog("directory create: %s", name);
			return DS_FILE_ERR;
		}
	}
	ds_verblog("%s: created directory", name);
	return 0;
}

static int link_create(const char *src, const char *linkpath, uint flags)
{
	if (flags & DS_FILE_LINK_HARD) {
		if (0 != fffile_hardlink(src, linkpath)) {
			ds_syserrlog("fffile_hardlink(): %s -> %s"
				, linkpath, src);
			return DS_FILE_ERR;
		}
		ds_verblog("created hard link: %s -> %s"
			, linkpath, src);
		return 0;
	}

	int r = fffile_symlink(src, linkpath);

	if (r != 0 && (flags & DS_FILE_CREATE)) {
		int e = fferr_last();
		if (0 != fffile_remove(linkpath)) {
			ds_syswarnlog("remove: %s", linkpath);
			fferr_set(e);
		} else {
			r = fffile_symlink(src, linkpath);
		}
	}

	if (r != 0) {
		ds_syserrlog("fffile_symlink(): %s -> %s"
			, linkpath, src);
		return DS_FILE_ERR;
	}

	ds_verblog("created symbolic link: %s -> %s"
		, linkpath, src);
	return 0;
}

static int file_move(ffstr old, ffstr _new, uint flags)
{
	if (flags & DS_FILE_MOVE_TEST) {
		ds_verblog("move (test): '%S' -> '%S'", &old, &_new);
		return 0;
	}

	int rc = DS_FILE_ERR;
	char *src = ffsz_dupstr(&old);
	char *dst = ffsz_dupstr(&_new);

	if (flags & DS_FILE_MOVE_SAFE) {
		fffd fd = fffile_open(dst, FFFILE_READONLY);
		if (fd != FFFILE_NULL) {
			fffile_close(fd);
			ds_errlog("move: target file already exists: %S", &_new);
			goto end;
		}
	}

	int r = fffile_rename(src, dst);
	if (r != 0 && fferr_notexist(fferr_last())) {
		if (0 != ffdir_make_path(dst, 0)) {
			ds_syserrlog("ffdir_make_path: for filename %s", dst);
			goto end;
		}
		r = fffile_rename(src, dst);
	}
	if (r != 0) {
		ds_syserrlog("move: %S -> %S", &old, &_new);
		goto end;
	}

	ds_verblog("moved: '%S' -> '%S'", &old, &_new);
	rc = 0;

end:
	ffmem_free(src);
	ffmem_free(dst);
	return rc;
}

static int file_delete(const char *name, uint flags)
{
	if (flags & DS_FILE_DEL_TEST) {
		ds_verblog("file delete (test): %s", name);
		return 0;
	}
	if (0 != fffile_remove(name)) {
		ds_syserrlog("file delete: %s", name);
		return DS_FILE_ERR;
	}
	ds_verblog("file deleted: %s", name);
	return 0;
}

const ds_file_if _ds_file = {
	file_create,
	file_destroy,
	file_open, file_close,
	file_read, file_write, file_write_fmt,
	file_flush,
	file_trunc,
	file_behaviour,
	file_info, file_fd,
	file_mtime_set, file_attr_set,
	dir_create,
	link_create,
	file_move,
	file_delete,
};
