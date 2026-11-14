/** dsynco: copy data (input -> output)
2022, Simon Zolin */

#include <dsynco.h>
#include <ffsys/path.h>

extern const ds_core *core;

#define ffmem_free0(p)  ffmem_free(p), (p) = NULL

#define BUF_LARGE  (8*1024*1024)

struct copy {
	struct ds_copy_conf conf;
	ds_task task;

	uint st;
	ffstr data;

	ds_file_obj*	input;
	const char	*iname;
	fffileinfo	fi;
	uint64		in_off;
	uint		nfiles;

	struct o_s {
		uint		state;
		ds_file_obj*f;
		uint64		total, off;
		const char	*name;
		char*		name_tmp;
		fffileinfo	fi;
		uint		del_on_close :1;
	} o;

	u_char preserve_date;
};

static int copy_input_next(struct copy *c)
{
	int r;
	if (c->nfiles)
		return 'done';
	const struct ds_copy_io *cio = ffslice_itemT(&c->conf.in_out, c->nfiles, struct ds_copy_io);
	c->nfiles++;
	c->iname = cio->in;

	uint flags = DS_FILE_READ | DS_FILE_READAHEAD | DS_FILE_INFO_NOFOLLOW;
	flags |= (c->conf.directio) ? DS_FILE_DIRECTIO : 0;
	r = core->file->open(c->input, c->iname, flags);
	if (r == DS_FILE_ERR) return 'erro';

	r = core->file->info(c->input, &c->fi);
	if (r == DS_FILE_ERR) return 'erro';
	return 0;
}

static void copy_complete(struct copy *c)
{
	ds_verblog("'%s' -> '%s'  [%,U]"
		, c->iname, c->o.name, c->o.total);
}

static void copy_run(void *param);
#include <fs/copy-output.h>

static void copy_free(struct copy *c)
{
	output_close(c);
	core->file->destroy(c->input);

	struct ds_copy_io *cio;
	FFSLICE_WALK(&c->conf.in_out, cio) {
		ffmem_free(cio->in);
		ffmem_free(cio->out);
	}
	ffmem_free(c);
}

static void copy_reset(struct copy *c)
{
	c->in_off = 0;
	output_reset(c);
}

static void copy_run(void *param)
{
	struct copy *c = param;
	int r, k = 0;
	enum {
		I_SRC, I_OPEN_OUT,
		I_READ, I_WRITE, I_RD_DONE, I_DONE,
	};
	while (1) {
		switch (c->st) {

		case I_SRC:
			copy_reset(c);
			switch (copy_input_next(c)) {
			case 'next':
				c->st = I_SRC;
				continue;

			case 'done':
				k = 1;
				goto end;

			case 'erro':
				goto end;
			}

			c->st = I_OPEN_OUT;
			continue;

		case I_OPEN_OUT:
			r = output_open(c);
			if (r == 'skip') {
				c->st = I_SRC;
				continue;
			}
			if (r != 0) goto end;

			core->file->behaviour(c->input, DS_FBEH_SEQ);

			c->st = I_READ;
			// fallthrough

		case I_READ:
			r = core->file->read(c->input, &c->data, c->in_off);
			if (r == DS_FILE_ERR) goto end;
			if (r == DS_FILE_EOF) {
				c->st = I_RD_DONE;
				continue;
			}
			c->in_off += c->data.len;

			c->st = I_WRITE;
			continue;

		case I_WRITE:
			if (output_write(c, c->data)) goto end;
			c->st = I_READ;
			continue;

		case I_RD_DONE:
			core->file->behaviour(c->o.f, DS_FBEH_TRUNC_PREALLOC);
			if (c->preserve_date) {
				core->file->mtime_set(c->o.f, fffileinfo_mtime1(&c->fi));
			}

			c->st = I_DONE;
			continue;

		case I_DONE:
			r = output_fin(c);
			if (r == 'asyn') return;
			if (r != 0) goto end;

			copy_complete(c);

			c->st = I_SRC;
			continue;
		}
	}

end:
	{
	void (*on_complete)(void*, int) = c->conf.on_complete;
	void *udata = c->conf.opaque;
	copy_free(c);
	int rc = (k) ? 0 : 1;
	ds_dbglog("copy: calling on_complete(): %d", rc);
	on_complete(udata, rc);
	}
}

int ds_copy_run(const struct ds_copy_conf *conf)
{
	if (!conf->in_out.len)
		return -1;

	struct copy *c = ffmem_new(struct copy);
	c->conf = *conf;
	if (!c->conf.buffer_size)
		c->conf.buffer_size = BUF_LARGE;
	c->preserve_date = 1;
	if (c->conf.update)
		c->conf.overwrite = 1;

	struct ds_file_conf fc = {};
	fc.buffer_size = c->conf.buffer_size;
	fc.n_buffers = 1;
	c->input = core->file->create(&fc);

	output_init(c);

	core->task(&c->task, copy_run, c);
	return 0;
}
