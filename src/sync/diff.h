/** dsynco: sync: compare directory trees
2022, Simon Zolin */

#include <ffsys/std.h>
#include <ffbase/murmurhash3.h>

struct fntree_cmp_ent {
	uint status; // enum FNTREE_CMP | enum DS_SYNC
	const fntree_entry *l, *r;
	const fntree_block *lb, *rb;
};

struct diff {
	fntree_cmp	fcmp;
	ffvec		ents; // fntree_cmp_ent[]
	ffvec		filter; // fntree_cmp_ent*[]
	ffmap		moved; // hash(name, struct ds_sync_entry) -> fntree_cmp_ent*
	ds_sync_snapshot *left, *right;
	ffvec		lname, rname; // NULL-terminated
	uint		options;
	uint		sort_flags;

	uint		dup_cur;
	uint64		sz_prev;
	struct fntree_cmp_ent *ce_prev;

	struct ds_sync_props props;
	struct ds_sync_diff_entry dif_ent;
	struct ds_sync_diff_stats stats;

	// TODO: this belongs in a separate ctx
	struct {
		uint have_result;
		int result;
		void (*on_event)(void*);
		void *udata;
	} sync;
};

void diff_close(struct diff *d)
{
	ffvec_free(&d->ents);
	ffvec_free(&d->filter);
	ffvec_free(&d->lname);
	ffvec_free(&d->rname);
	ffmap_free(&d->moved);
}

/** Compare 2 files */
static int diff_data_cmp(void *opaque, const fntree_entry *l, const fntree_entry *r)
{
	ffstr lname = fntree_name(l), rname = fntree_name(r);
	int i = ffstr_icmp2(&lname, &rname);
	if (i < 0)
		return FNTREE_CMP_LEFT;
	else if (i > 0)
		return FNTREE_CMP_RIGHT;

	struct diff *sd = (struct diff*)opaque;
	const struct ds_sync_entry *ld = (struct ds_sync_entry*)fntree_data(l)
		, *rd = (struct ds_sync_entry*)fntree_data(r);

	uint k = FNTREE_CMP_EQ;

	if (ld->size != rd->size) {
		uint f = (ld->size < rd->size) ? DS_SYNC_SMALLER : DS_SYNC_LARGER;
		k |= FNTREE_CMP_NEQ | f;
	}

	if (!(sd->options & DS_SYNC_DIFF_NO_ATTR)
		&& (ld->unix_attr != rd->unix_attr
			|| ld->win_attr != rd->win_attr))
		k |= FNTREE_CMP_NEQ | DS_SYNC_ATTR;

	if (!(sd->options & DS_SYNC_DIFF_NO_TIME)) {
		int i;
		if ((sd->options & DS_SYNC_DIFF_TIME_2SEC)
			&& ld->mtime.sec/2 == rd->mtime.sec/2)
		{}
		else if ((i = fftime_cmp(&ld->mtime, &rd->mtime))) {
			uint f = (i < 0) ? DS_SYNC_OLDER : DS_SYNC_NEWER;
			k |= FNTREE_CMP_NEQ | f;
		}
	}

	return k;
}

static int diff_moved_check_data(struct diff *sd, const struct fntree_cmp_ent *ce)
{
	sync_full_name(&sd->lname, ce->l, ce->lb);
	sync_full_name(&sd->rname, ce->r, ce->rb);
	const char *ln = (const char*)sd->lname.ptr, *rn = (const char*)sd->rname.ptr;
	ds_dbglog("comparing files '%s' and '%s'", ln, rn);
	int r = file_cmp(ln, rn, 4096, FILE_CMP_FAST);
	if (r < 0)
		ds_errlog("comparing files '%s' and '%s'", ln, rn);
	return !r;
}

/** Return 1 if two files match by name and meta */
static int diff_moved_keyeq(void *opaque, const void *key, ffsize keylen, void *val)
{
	struct diff *sd = (struct diff*)opaque;
	const fntree_entry *r = (fntree_entry*)key;
	const struct fntree_cmp_ent *ce = (struct fntree_cmp_ent*)val;
	const fntree_entry *l = ce->l;
	if (l == NULL)
		l = ce->r;
	const struct ds_sync_entry *ld = (void*)fntree_data(l)
		, *rd = (void*)fntree_data(r);

	return ld->size == rd->size
		&& (ld->unix_attr & FFFILE_UNIX_DIR) == (rd->unix_attr & FFFILE_UNIX_DIR)

		&& ((sd->options & DS_SYNC_DIFF_MOVE_NO_NAME)
			|| (l->name_len == r->name_len
				&& !ffmem_cmp(l->name, r->name, l->name_len)))

		&& ((sd->options & DS_SYNC_DIFF_NO_TIME)
			|| fftime_to_msec(&ld->mtime) == fftime_to_msec(&rd->mtime))

		&& (!(sd->options & DS_SYNC_DIFF_MOVE_CHK_CONTENT)
			|| (ld->unix_attr & FFFILE_UNIX_DIR)
			|| diff_moved_check_data(sd, ce))
		;
}

uint diff_ent_moved_hash(struct diff *sd, fntree_entry *e)
{
	const struct ds_sync_entry *d = (struct ds_sync_entry*)fntree_data(e);
	struct {
		char type;
		char mtime_msec[8];
		char size[8];
		char name[1024];
	} key;

	key.type = 0;
	if (!(sd->options & DS_SYNC_DIFF_NO_ATTR))
		key.type = !!(d->unix_attr & FFFILE_UNIX_DIR);

	*(uint64*)key.mtime_msec = 0;
	if (!(sd->options & DS_SYNC_DIFF_NO_TIME))
		*(uint64*)key.mtime_msec = fftime_to_msec(&d->mtime);

	*(uint64*)key.size = d->size;

	uint n = 1+8+8;
	if (!(sd->options & DS_SYNC_DIFF_MOVE_NO_NAME))
		n += ffmem_ncopy(key.name, sizeof(key.name), e->name, e->name_len);

	return murmurhash3(&key, n, 0x789abcde);
}

/** Find (and add if not found) unique file to a Moved table */
struct fntree_cmp_ent* diff_moved_find_add(struct diff *sd, fntree_entry *e, struct fntree_cmp_ent *ce)
{
	uint hash = diff_ent_moved_hash(sd, e);
	ffstr name = fntree_name(e);
	struct fntree_cmp_ent *it = (struct fntree_cmp_ent*)ffmap_find_hash(&sd->moved, hash, e, sizeof(void*), sd);
	ds_dbglog("move: found:%u  hash:%xu  '%S'"
		, (!!it), hash, &name);
	if (it == NULL) {
		// store this file in Moved table
		ffmap_add_hash(&sd->moved, hash, ce);
	}
	return it;
}

/** Prepare to analyze the difference */
void diff_init(struct diff *sd, ds_sync_snapshot *left, ds_sync_snapshot *right, uint flags)
{
	sd->options = flags;
	sd->left = left;
	sd->right = right;
	ffmap_init(&sd->moved, diff_moved_keyeq);

	const fntree_block *l = left->root, *r = right->root;
	if (flags & DS_SYNC_DIFF_LEFT_PATH_STRIP)
		l = _fntr_ent_first(left->root)->children;
	if (flags & DS_SYNC_DIFF_RIGHT_PATH_STRIP)
		r = _fntr_ent_first(right->root)->children;

	fntree_cmp_init(&sd->fcmp, l, r, diff_data_cmp, sd);
	ffvec_allocT(&sd->ents, left->total + right->total, struct fntree_cmp_ent);
	sd->stats.ltotal = left->total;
	sd->stats.rtotal = right->total;
}

/** Compare next file pair */
int diff_next(struct diff *sd)
{
	fntree_entry *le, *re;
	fntree_block *lb, *rb;
	int r = fntree_cmp_next(&sd->fcmp, &le, &re, &lb, &rb);
	if (r < 0) {
		return 1;
	}

	// unset the unused elements
	switch (r & 0x0f) {
	case FNTREE_CMP_LEFT:
		re = NULL, rb = NULL; break;
	case FNTREE_CMP_RIGHT:
		le = NULL, lb = NULL; break;
	}

	FF_ASSERT(sd->ents.len < sd->ents.cap);
	struct fntree_cmp_ent *ce = ffvec_pushT(&sd->ents, struct fntree_cmp_ent);
	ce->status = 0;
	ce->l = le;
	ce->r = re;
	ce->lb = lb;
	ce->rb = rb;

	sync_full_name(&sd->lname, le, lb);
	sync_full_name(&sd->rname, re, rb);

	ds_dbglog("%S <-> %S: %xu", &sd->lname, &sd->rname, r);

	switch (r & 0x0f) {
	case FNTREE_CMP_LEFT: {
		sd->stats.left++;

		struct fntree_cmp_ent *ce2 = diff_moved_find_add(sd, le, ce);
		if (ce2 && (ce2->status & DS_SYNC_RIGHT)) {
			// the same file was seen before within Right list
			ds_dbglog("moved R: %S", &sd->lname);
			ce2->status &= ~DS_SYNC_RIGHT;
			ce2->status |= DS_SYNC_MOVE;
			ce->status = DS_SYNC_MOVE | _DS_SYNC_SKIP;
			ce2->l = le;
			ce2->lb = lb;
			sd->stats.moved++;
			sd->stats.left--;
			sd->stats.right--;
			sd->stats.entries--;
		} else {
			ce->status = DS_SYNC_LEFT;
		}
		break;
	}

	case FNTREE_CMP_RIGHT: {
		sd->stats.right++;

		struct fntree_cmp_ent *ce2 = diff_moved_find_add(sd, re, ce);
		if (ce2 && (ce2->status & DS_SYNC_LEFT)) {
			// the same file was seen before within Left list
			ds_dbglog("moved L: %S", &sd->rname);
			ce2->status &= ~DS_SYNC_LEFT;
			ce2->status |= DS_SYNC_MOVE;
			ce->status = DS_SYNC_MOVE | _DS_SYNC_SKIP;
			ce2->r = re;
			ce2->rb = rb;
			sd->stats.moved++;
			sd->stats.left--;
			sd->stats.right--;
			sd->stats.entries--;
		} else {
			ce->status = DS_SYNC_RIGHT;
		}
		break;
	}

	case FNTREE_CMP_EQ:
		sd->stats.eq++;
		ce->status = DS_SYNC_EQ;
		break;

	case FNTREE_CMP_NEQ:
		sd->stats.neq++;
		ce->status = DS_SYNC_NEQ | (r & ~0x0f);
		break;

	default:
		DS_ASSERT(0);
		return 1;
	}

	sd->stats.entries++;
	return 0;
}

static int diff_sort_f(const void *_a, const void *_b, void *udata)
{
	ds_sync_diff *sd = (ds_sync_diff*)udata;
	const struct fntree_cmp_ent *a = *(struct fntree_cmp_ent**)_a,  *b = *(struct fntree_cmp_ent**)_b;
	const struct ds_sync_entry *d;
	uint64 as = 0, bs = 0;
	fftime at = {}, bt = {};

	if (a->l) {
		d = (struct ds_sync_entry*)fntree_data(a->l);
		as = d->size;
		at = d->mtime;
	}
	if (a->r) {
		d = (struct ds_sync_entry*)fntree_data(a->r);
		as = ffmax(as, d->size);
		if (fftime_cmp(&at, &d->mtime) < 0)
			at = d->mtime;
	}

	if (b->l) {
		d = (struct ds_sync_entry*)fntree_data(b->l);
		bs = d->size;
		bt = d->mtime;
	}
	if (b->r) {
		d = (struct ds_sync_entry*)fntree_data(b->r);
		bs = ffmax(bs, d->size);
		if (fftime_cmp(&bt, &d->mtime) < 0)
			bt = d->mtime;
	}

	int rs = (as > bs) ? -1
		: (as < bs) ? 1
		: 0;
	if (sd->sort_flags & DS_SYNC_SORT_FILESIZE)
		return rs;

	int rt = (fftime_cmp(&at, &bt) > 0) ? -1
		: (fftime_cmp(&at, &bt) < 0) ? 1
		: 0;
	if (sd->sort_flags & DS_SYNC_SORT_MTIME)
		return rt;

	return 0;
}

/** Prepare diff list; sort (filter) files by size. */
void diff_dups_init(struct diff *sd, ds_sync_snapshot *left, uint flags)
{
	sd->sz_prev = ~0ULL;
	sd->options = flags | _DS_SYNC_DIFF_DUPS;
	sd->left = left;
	fntree_block *b = _fntr_ent_first(left->root)->children;
	if (!b)
		return;
	ffvec_allocT(&sd->ents, left->total, struct fntree_cmp_ent);
	ffvec_allocT(&sd->filter, left->total, void*);
	sd->stats.ltotal = left->total;

	fntree_cursor cur = {};
	fntree_entry *e;
	while ((e = fntree_cur_next_r(&cur, &b))) {
		FF_ASSERT(sd->ents.len < sd->ents.cap);
		struct fntree_cmp_ent *ce = ffvec_zpushT(&sd->ents, struct fntree_cmp_ent);
		ce->l = e;
		ce->lb = b;

		*ffvec_pushT(&sd->filter, void*) = ce;
	}

	sd->sort_flags = DS_SYNC_SORT_FILESIZE;
	ffsort(sd->filter.ptr, sd->filter.len, sizeof(void*), diff_sort_f, sd);
}

/** Walk through the files and compare by content.
 * Associate duplicate files with each other.
Set 'EQ' status for the duplicate files; set 'NEQ' otherwise. */
int diff_dups_scan_next(struct diff *sd)
{
	if (sd->dup_cur == sd->filter.len) {
		if (sd->ce_prev) {
			sd->stats.neq++;
			sd->ce_prev->status = DS_SYNC_NEQ;
		}
		sd->filter.len = 0;
		return 1;
	}

	uint64 sz;
	struct fntree_cmp_ent *ce;
	ce = *ffslice_itemT(&sd->filter, sd->dup_cur++, struct fntree_cmp_ent*);
	sd->stats.entries++;
	struct ds_sync_entry *d = (struct ds_sync_entry*)fntree_data(ce->l);
	sz = d->size;
	if (sd->sz_prev != sz) {
		sd->sz_prev = sz;
		if (sd->ce_prev) {
			sd->stats.neq++;
			sd->ce_prev->status = DS_SYNC_NEQ;
		}
		sd->ce_prev = ce;
		return 0;
	}

	sync_full_name(&sd->lname, sd->ce_prev->l, sd->ce_prev->lb);
	sync_full_name(&sd->rname, ce->l, ce->lb);
	ds_dbglog("comparing files \"%s\" and \"%s\"..."
		, (const char*)sd->lname.ptr, (const char*)sd->rname.ptr);
	if (file_cmp((const char*)sd->lname.ptr, (const char*)sd->rname.ptr, 4096, FILE_CMP_FAST)) {
		sd->stats.neq++;
		sd->ce_prev->status = DS_SYNC_NEQ;
		sd->ce_prev = ce;
		return 0; // Note: only 2 consecutive files with the same size are compared!
	}
	sd->stats.eq++;
	sd->ce_prev->status = DS_SYNC_EQ;
	sd->ce_prev->r = ce->l;
	sd->ce_prev->rb = ce->lb;
	ce->status = DS_SYNC_DONE;
	sd->ce_prev = NULL;
	sd->sz_prev = ~0ULL;
	return 0;
}

static ds_sync_diff* sync_diff(ds_sync_snapshot *left, ds_sync_snapshot *right, struct ds_sync_props *props, uint flags)
{
	struct diff *sd = ffmem_new(struct diff);
	sd->props = *props;
	diff_init(sd, left, right, flags);
	if (!(flags & DS_SYNC_DIFF_STEP)) {
		while (!diff_next(sd)) {
		}
	}
	props->stats = sd->stats;
	return sd;
}

static int sync_diff_next(ds_sync_diff *sd, struct ds_sync_props *props)
{
	int r = !(sd->options & _DS_SYNC_DIFF_DUPS) ? diff_next(sd) : diff_dups_scan_next(sd);
	props->stats = sd->stats;
	return r;
}

static ds_sync_diff* sync_find_dups(ds_sync_snapshot *left, struct ds_sync_props *props, uint flags)
{
	struct diff *sd = ffmem_new(struct diff);
	diff_dups_init(sd, left, flags);
	if (!(flags & DS_SYNC_DIFF_STEP)) {
		while (!diff_dups_scan_next(sd)) {}
	}
	props->stats = sd->stats;
	return sd;
}

static void sync_diff_free(ds_sync_diff *sd)
{
	if (sd)
		diff_close(sd);
	ffmem_free(sd);
}

static int str_match(const fntree_block *b, const fntree_entry *e, ffstr m)
{
	if (!e)
		return 0;
	ffstr path = fntree_path(b);
	ffstr name = fntree_name(e);
	return (ffstr_ifindstr(&name, &m) >= 0
		|| ffstr_ifindstr(&path, &m) >= 0);
}

static uint flags_swap(uint u, uint a, uint b)
{
	if (u & a)
		u = (u & ~a) | b;
	else if (u & b)
		u = (u & ~b) | a;
	return u;
}

static uint status_swap(uint st)
{
	st = flags_swap(st, DS_SYNC_LEFT, DS_SYNC_RIGHT);
	st = flags_swap(st, DS_SYNC_NEWER, DS_SYNC_OLDER);
	st = flags_swap(st, DS_SYNC_LARGER, DS_SYNC_SMALLER);
	return st;
}

static uint sync_view(ds_sync_diff *sd, struct ds_sync_props *props, uint flags)
{
	sd->filter.len = 0;
	struct fntree_cmp_ent *ce;
	FFSLICE_WALK(&sd->ents, ce) {

		if (ce->status & _DS_SYNC_SKIP)
			continue;

		if (!(flags & DS_SYNC_DONE) && (ce->status & DS_SYNC_DONE))
			continue;

		uint st = ce->status;
		if (flags & DS_SYNC_SWAP)
			st = status_swap(ce->status);

		switch (st & DS_SYNC_MASK) {
		case DS_SYNC_LEFT:
			if (!(flags & DS_SYNC_LEFT))
				continue;
			break;

		case DS_SYNC_RIGHT:
			if (!(flags & DS_SYNC_RIGHT))
				continue;
			break;

		case DS_SYNC_NEQ:
			if (!(flags & DS_SYNC_NEQ))
				continue;
			// TODO
			// if (!(flags & (DS_SYNC_NEWER | DS_SYNC_OLDER))
			// 	&& !(st & (DS_SYNC_LARGER | DS_SYNC_SMALLER | DS_SYNC_ATTR)))
			// 	continue;
			break;

		case DS_SYNC_MOVE:
			if (!(flags & DS_SYNC_MOVE))
				continue;
			break;

		case DS_SYNC_EQ:
			if (!(flags & DS_SYNC_EQ))
				continue;
			break;
		}

		const struct ds_sync_entry *ld = NULL, *rd = NULL;
		if (ce->l)
			ld = (void*)fntree_data(ce->l);
		if (ce->r)
			rd = (void*)fntree_data(ce->r);

		if (!(flags & DS_SYNC_DIR)
			&& ((ld && (ld->unix_attr & FFFILE_UNIX_DIR))
				|| (rd && (rd->unix_attr & FFFILE_UNIX_DIR))))
			continue; // Skip dirs

		if (props->since_time.sec) {
			if ((!ld || fftime_cmp(&ld->mtime, &props->since_time) < 0)
				&& (!rd || fftime_cmp(&rd->mtime, &props->since_time) < 0))
				continue;
		}

		uint included = !props->include.len;

		ffstr *s;
		FFSLICE_WALK(&props->exclude, s) {
			if (str_match(ce->lb, ce->l, *s)
				|| str_match(ce->rb, ce->r, *s))
				goto next;
		}

		if (!props->include_both) {
			FFSLICE_WALK(&props->include, s) {
				if (str_match(ce->lb, ce->l, *s)
					|| str_match(ce->rb, ce->r, *s)) {
					included = 1;
					break;
				}
			}

		} else {
			FFSLICE_WALK(&props->include, s) {
				if (str_match(ce->lb, ce->l, *s)
					&& str_match(ce->rb, ce->r, *s)) {
					included = 1;
					break;
				}
			}
		}

		if (!included)
			continue;

		*ffvec_pushT(&sd->filter, void*) = ce;

	next:
		;
	}

	return sd->filter.len;
}

static uint sync_sort(ds_sync_diff *sd, uint flags)
{
	sd->sort_flags = flags;
	ffsort(sd->filter.ptr, sd->filter.len, sizeof(void*), diff_sort_f, sd);
	return sd->filter.len;
}

#define FF_SWAP2(a, b) \
({ \
	__typeof__(a) __tmp = a; \
	a = b; \
	b = __tmp; \
})

static int sync_info(ds_sync_diff *sd, void *id, uint flags, struct ds_sync_diff_entry *dst)
{
	struct fntree_cmp_ent *ce;
	if (flags & DS_SYNC_INFO_INDEX) {
		uint i = (size_t)id;
		if (i >= sd->filter.len)
			return -1;
		ce = *ffslice_itemT(&sd->filter, i, struct fntree_cmp_ent*);
	} else {
		ce = (struct fntree_cmp_ent*)id;
	}

	ffmem_zero_obj(dst);
	dst->status = ce->status;
	ffvec lname = {}, rname = {};
	sync_full_name(&lname, ce->l, ce->lb);
	sync_full_name(&rname, ce->r, ce->rb);
	dst->lname = *(ffstr*)&lname;
	dst->rname = *(ffstr*)&rname;
	dst->left = (ce->l) ? (struct ds_sync_entry*)fntree_data(ce->l) : NULL;
	dst->right = (ce->r) ? (struct ds_sync_entry*)fntree_data(ce->r) : NULL;

	if (flags & DS_SYNC_INFO_SWAP) {
		dst->status = status_swap(ce->status);
		FF_SWAP2(dst->lname, dst->rname);
		FF_SWAP2(dst->left, dst->right);
	}

	dst->id = ce;
	return 0;
}

static uint sync_status(ds_sync_diff *sd, void *id, uint mask, uint val)
{
	struct fntree_cmp_ent *ce = (struct fntree_cmp_ent*)id;
	ce->status = (ce->status & ~mask) | val;
	return ce->status;
}
