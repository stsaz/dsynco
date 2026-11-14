/** dsynco: public interface for inter-module communication */

#pragma once
#include <ffsys/error.h>
#include <util/taskqueue.h>
#include <ffsys/file.h>
#include <ffsys/timerqueue.h>
#include <ffsys/time.h>
#include <ffbase/vector.h>
#include <assert.h>

typedef unsigned char u_char;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long long uint64;
typedef long long int64;

// CORE

#define DS_ASSERT  assert

enum DS_LOG {
	DS_LOG_ERR,
	DS_LOG_WARN,
	DS_LOG_INFO,
	DS_LOG_VERBOSE,
	DS_LOG_DBG,
	DS_LOG_SYSERR = 0x10,
};

/**
flags: enum DS_LOG */
typedef void (*ds_log_func)(uint flags, const char *fmt, va_list args);

typedef struct ds_core ds_core;

struct ds_core_conf {
	uint timer_resolution_msec;
	char *app_path;
	ds_log_func log;
	uint debug :1;
	uint verbose :1;
	uint stdout_color :1;
};

/** Core initializer.
libdsynco.so exports this interface as "ds_coreinit" */
struct ds_coreinit {
	void (*init)();
	void (*destroy)();
	ds_core* (*conf)(struct ds_core_conf *conf);
	int (*run)();
};

typedef struct ds_file_if ds_file_if;
typedef struct ds_sync_if ds_sync_if;
typedef fftask ds_task;
typedef fftimerqueue_node ds_timer;
typedef void (*ds_task_func)(void *param);

typedef struct ds_kevent {
	ds_task_func func;
	void *param;
} ds_kevent;

static inline void ds_kevent_set(ds_kevent *kev, ds_task_func func, void *param)
{
	kev->func = func;
	kev->param = param;
}

enum DS_CORE_CLOCK {
	DS_CORE_MONOTONIC,
	DS_CORE_UTC,
};

/** Core runtime interface */
struct ds_core {
	/** File submodule interface */
	const ds_file_if *file;
	/** Sync submodule interface */
	const ds_sync_if *sync_if;
	/** Version string */
	const char *version_str;
	fftime_zone tz;
	const char **env;

	/** Exit from ds_coreinit.run() */
	void (*exit)(int exit_code);

	/** Get absolute path for files in dsynco/ directory.
	Return newly allocated string, free with ffmem_free(). */
	char* (*path)(const char *name);

	/** Add/remove asynchronous task */
	void (*task)(ds_task *task, ds_task_func func, void *param);

	/** Set/disable timer.
	interval_msec:
	 >0: periodic
	 <0: one-shot
	 =0: disable */
	void (*timer)(ds_timer *timer, int interval_msec, ds_task_func func, void *param);

	/** Get current time
	dt: Optional
	flags: enum DS_CORE_CLOCK */
	fftime (*clock)(ffdatetime *dt, uint flags);

	/** Print log message */
	void (*log)(uint flags, const char *fmt, ...);
	void (*logv)(uint flags, const char *fmt, va_list args);

	uint debug :1;
	uint verbose :1;
	uint stdout_color :1;
};

#define ds_syserrlog(fmt, ...)  (core)->log(DS_LOG_ERR | DS_LOG_SYSERR, fmt, ##__VA_ARGS__)
#define ds_errlog(fmt, ...)  (core)->log(DS_LOG_ERR, fmt, ##__VA_ARGS__)
#define ds_warnlog(fmt, ...)  (core)->log(DS_LOG_WARN, fmt, ##__VA_ARGS__)
#define ds_syswarnlog(fmt, ...)  (core)->log(DS_LOG_WARN | DS_LOG_SYSERR, fmt, ##__VA_ARGS__)
#define ds_infolog(fmt, ...)  (core)->log(DS_LOG_INFO, fmt, ##__VA_ARGS__)
#define ds_verblog(fmt, ...) \
	do { if (core->verbose) (core)->log(DS_LOG_VERBOSE, fmt, ##__VA_ARGS__); } while (0)
#define ds_dbglog(fmt, ...) \
	do { if (core->debug) (core)->log(DS_LOG_DBG, fmt, ##__VA_ARGS__); } while (0)


// COMMAND

struct ds_sync_conf {
	ffvec	input; // ffstr[], source paths
	ffstr	output; // target path
	ffslice include, exclude; // ffstr[], case-insensitive substring filters

	uint	diff_flags; // enum DS_SYNC, display filter; non-zero: diff-only (no sync)
	uint	recent_days; // skip files older than N days on both sides

	uint	diff_full_name :1; // show full paths (don't strip root)
	uint	diff_no_attr :1; // ignore file attributes when comparing
	uint	diff_no_time :1; // ignore mtime when comparing
	uint	diff_time_2sec :1; // treat mtime as equal within 2-second resolution
	uint	plain_list :1; // print names only (no status/size)
	uint	sync_add :1; // copy files only in source
	uint	sync_del :1; // delete files only in target
	uint	sync_move :1; // move files detected as moved/renamed
	uint	sync_update :1; // overwrite files that differ

	uint	atomic :1; // write to temp file, trash old, rename
	uint	replace_date :1; // if target exists: update its mtime, don't copy data
	uint	test :1; // dry-run

	/** The function to call after the operation completes */
	void (*on_complete)(void*, int);
	void *opaque;
};

FF_EXTERN int ds_sync_run(const struct ds_sync_conf *conf);

struct ds_copy_io {
	char *in, *out;
};

struct ds_copy_conf {
	ffvec in_out; // struct ds_copy_io[]
	uint buffer_size; // bytes; 0: default
	uint replace_date :1; // if target exists: update its mtime, don't copy data
	uint update :1;		// if target exists: overwrite if source is newer
	uint atomic :1;		// write to temp file, trash old, rename
	uint overwrite :1;	// if target exists: overwrite
	uint test :1;		// dry-run
	uint directio :1;	// use direct I/O if available

	/** The function to call after the operation completes */
	void (*on_complete)(void*, int);
	void *opaque;
};

FF_EXTERN int ds_copy_run(const struct ds_copy_conf *conf);

struct ds_trash_conf {
	ffvec input; // ffstr[], paths to trash
	uint delete_on_fail :1; // on trash failure: hard-delete the files (skipped if test=1)
	uint test :1; // dry-run

	/** The function to call after the operation completes */
	void (*on_complete)(void*, int);
	void *opaque;
};

FF_EXTERN int ds_trash_run(const struct ds_trash_conf *conf);


// FILE

struct ds_file_conf {
	uint buffer_size;
	uint n_buffers;
};

enum DS_FILE_OPEN {
	DS_FILE_READ,
	DS_FILE_WRITE = 1,
	DS_FILE_READWRITE = 2,

	DS_FILE_CREATENEW = 4,
	DS_FILE_CREATE = 8,
	DS_FILE_CREATE_TRUNC = DS_FILE_CREATE | 0x1000,

	DS_FILE_DIRECTIO = 0x40,
	DS_FILE_FAKEWRITE = 0x80,
	DS_FILE_NO_PREALLOC = 0x0100,
	DS_FILE_INFO_NOFOLLOW = 0x0200, // info(): don't follow symlinks
	DS_FILE_READAHEAD = 0x0400, // Windows: enable "read-ahead"
	DS_FILE_LINK_HARD = 0x0800, // link(): create hard link (default: symbolic)
};

enum DS_FILE_RET {
	DS_FILE_OK,
	DS_FILE_EOF,
	DS_FILE_ASYNC,
	DS_FILE_ERR,
};

enum DS_FILE_BEH {
	DS_FBEH_SEQ, // Sequential access behaviour
	DS_FBEH_RANDOM, // Random access behaviour
	DS_FBEH_TRUNC_PREALLOC, // Truncate the currently preallocated space
};

enum DS_FILE_FD {
	DS_FILE_GET,
	DS_FILE_ACQUIRE, // Acquire descriptor.  Only open() can be called afterwards.
};

enum DS_FILE_MOVE_F {
	DS_FILE_MOVE_SAFE = 1, // Fail if target file already exists
	DS_FILE_MOVE_TEST = 2, // Dry-run: do not perform the move
};

enum DS_FILE_DEL_F {
	DS_FILE_DEL_TEST = 1, // Dry-run: do not delete the file
};

enum DS_FILE_DIR_F {
	DS_FILE_DIR_RECURSIVE = 1, // Recursively create parent directories if necessary
};

typedef void ds_file_obj;

/** Bufferred file I/O interface.
This interface allows:
* Completely asynchronous file operations
* User-space data caching on reading and writing
* Controlling kernel caching behaviour */
struct ds_file_if {
	/** Allocate file object.
	conf: buffer configuration
	Return: file object handle */
	ds_file_obj* (*create)(struct ds_file_conf *conf);

	/** Destroy file object and free resources. */
	void (*destroy)(ds_file_obj *f);

	/** Open file (creates missing parent directories in write mode).
	f: file object handle
	name: file path
	flags: enum DS_FILE_OPEN
	Return enum DS_FILE_RET */
	int (*open)(ds_file_obj *f, const char *name, uint flags);

	/** Close file. */
	void (*close)(ds_file_obj *f);

	/** Read data from file.
	f: file object handle
	d: output buffer
	off: byte offset (-1: current offset)
	Return: enum DS_FILE_RET */
	int (*read)(ds_file_obj *f, ffstr *d, int64 off);

	/** Write data to file.
	f: file object handle
	d: data to write
	off: byte offset (-1: current offset)
	Return: enum DS_FILE_RET */
	int (*write)(ds_file_obj *f, ffstr d, int64 off);

	/** Write formatted data to file.
	f: file object handle
	fmt: format string, ...
	Return: enum DS_FILE_RET */
	int (*write_fmt)(ds_file_obj *_f, const char *fmt, ...);

	/** Flush buffered data to disk.
	f: file object handle
	Return: enum DS_FILE_RET */
	int (*flush)(ds_file_obj *f);

	/** Truncate or extend file.
	f: file object handle
	size: new file size in bytes (-1: current offset)
	Return: enum DS_FILE_RET */
	int (*trunc)(ds_file_obj *f, int64 size);

	/** Set file access behaviour hint.
	f: file object handle
	flags: enum DS_FILE_BEH
	Return: enum DS_FILE_RET */
	int (*behaviour)(ds_file_obj *f, uint flags);

	/** Get file info.
	Respects DS_FILE_INFO_NOFOLLOW (set in open).
	f: file object handle
	fi: output file info
	Return: enum DS_FILE_RET */
	int (*info)(ds_file_obj *f, fffileinfo *fi);

	/** Get OS-level file descriptor.
	f: file object handle
	flags: enum DS_FILE_FD
	Return: OS file descriptor */
	fffd (*fd)(ds_file_obj *f, uint flags);

	/** Set modification time (applied on close()).
	f: file object handle
	mtime: time since year 1
	Return: enum DS_FILE_RET */
	int (*mtime_set)(ds_file_obj *f, fftime mtime);

	/** Set file attributes.
	f: file object handle
	attr: file attributes
	Return: enum DS_FILE_RET */
	int (*attr_set)(ds_file_obj *f, uint attr);

	/** Create directory (succeeds if it already exists).
	name: directory path
	flags: enum DS_FILE_DIR_F
	Return: enum DS_FILE_RET */
	int (*dir_create)(const char *name, uint flags);

	/** Create link.
	src: target path (existing file for hard link, or link target for symbolic link)
	linkpath: new link path
	flags:
		DS_FILE_LINK_HARD: hard link (default: symbolic);
		DS_FILE_CREATE: overwrite existing link (symbolic only)
	Return: enum DS_FILE_RET */
	int (*link)(const char *src, const char *linkpath, uint flags);

	/** Move file (creates missing parent directories for destination).
	old: source path
	_new: destination path
	flags: enum DS_FILE_MOVE_F
	Return: enum DS_FILE_RET */
	int (*move)(ffstr old, ffstr _new, uint flags);

	/** Delete file.
	name: file path
	flags: enum DS_FILE_DEL_F
	Return: enum DS_FILE_RET */
	int (*del)(const char *name, uint flags);
};

static inline fftime fffileinfo_mtime1(const fffileinfo *fi)
{
	fftime t = fffileinfo_mtime(fi);
	t.sec += FFTIME_1970_SECONDS;
	return t;
}


// SYNC

enum DS_SYNC_SCAN {
	DS_SYNC_SCAN_STEP = 1, // step mode: caller drives scan_next()
	DS_SYNC_SCAN_WILDCARD = 2, // wildcard mode: full scan, no step
};

enum DS_SYNC_DIFF {
	DS_SYNC_DIFF_LEFT_PATH_STRIP = 1,
	DS_SYNC_DIFF_RIGHT_PATH_STRIP = 2,
	DS_SYNC_DIFF_NO_ATTR = 8,
	DS_SYNC_DIFF_NO_TIME = 0x10,
	DS_SYNC_DIFF_TIME_2SEC = 0x20,
	DS_SYNC_DIFF_MOVE_NO_NAME = 0x40,
	DS_SYNC_DIFF_MOVE_CHK_CONTENT = 0x80,
	DS_SYNC_DIFF_STEP = 0x0100,
	_DS_SYNC_DIFF_DUPS = 0x0200,
};

enum DS_SYNC_INFO {
	DS_SYNC_INFO_INDEX = 1, // id is an index into filtered list
	DS_SYNC_INFO_SWAP = 0x4000, // swap left/right (= DS_SYNC_SWAP)
};

enum DS_SYNC {
	DS_SYNC_LEFT = 1,
	DS_SYNC_RIGHT = 2,
	DS_SYNC_NEQ = 4,
	DS_SYNC_MOVE = 8,
	DS_SYNC_EQ = 0x10,
	DS_SYNC_MASK = 0xff,

	DS_SYNC_NEWER = 0x0100,
	DS_SYNC_OLDER = 0x0200,

	DS_SYNC_LARGER = 0x0400,
	DS_SYNC_SMALLER = 0x0800,

	DS_SYNC_ATTR = 0x1000,

	DS_SYNC_DIR = 0x2000,
	DS_SYNC_SWAP = 0x4000,
	_DS_SYNC_SKIP = 0x8000, // moved-double

	// user:
	DS_SYNC_SYNCING = 0x010000,
	DS_SYNC_ERROR = 0x020000,
	DS_SYNC_DONE = 0x040000,
};

enum DS_SYNC_SYNC {
	DS_SYNC_SYNC_REPLACE_DATE = 1, // if target exists: update its mtime, skip copy
	DS_SYNC_SYNC_VERIFY = 2, // verify file after copy
	DS_SYNC_SYNC_ATOMIC = 4, // write to temp, trash old, rename
	DS_SYNC_SYNC_FILE_DEL = 8, // delete files instead of moving to Trash
	DS_SYNC_SYNC_TEST = 0x10, // dry-run
	// DS_SYNC_SWAP
};

enum DS_SYNC_SORT {
	DS_SYNC_SORT_FILESIZE = 1,
	DS_SYNC_SORT_MTIME = 2,
};

struct ds_sync_diff_stats {
	uint eq, left, right, neq, moved;
	uint ltotal, rtotal, entries;
};

struct ds_sync_props {
	ffslice include, exclude; // ffstr[]
	fftime since_time;
	uint include_both :1;

	struct ds_sync_diff_stats stats;
};

struct ds_sync_entry {
	uint64	size;
	uint	unix_attr, win_attr;
	uint	uid, gid;
	fftime	mtime; // UTC
	uint	crc32;
};

struct ds_sync_diff_entry {
	uint status; // enum DS_SYNC
	ffstr lname, rname; // NULL-terminated
	struct ds_sync_entry *left, *right;
	void *id;
};

static inline void ds_sync_diff_entry_destroy(struct ds_sync_diff_entry *de) {
	ffstr_free(&de->lname);
	ffstr_free(&de->rname);
}

typedef struct snapshot ds_sync_snapshot;
typedef struct diff ds_sync_diff;
typedef struct ds_sync_if ds_sync_if;
struct ds_sync_if {
	/** Scan directory (or wildcard pattern) and create snapshot.
	path: directory path, or path with wildcard in the last component
	flags: enum DS_SYNC_SCAN
	Return: snapshot handle, NULL on error */
	ds_sync_snapshot* (*scan)(ffstr path, uint flags);

	/** Read next entry from snapshot.
	ss: snapshot handle
	Return: 0: got entry, 1: no more entries, -1: error */
	int (*scan_next)(ds_sync_snapshot *ss);

	/** Free snapshot. */
	void (*snapshot_free)(ds_sync_snapshot *ss);

	/** Compare two snapshots.
	left, right: snapshots
	props: stored in diff; stats written back on completion
	flags: enum DS_SYNC_DIFF
	Return: diff handle */
	ds_sync_diff* (*diff)(ds_sync_snapshot *left, ds_sync_snapshot *right, struct ds_sync_props *props, uint flags);

	/** Read next diff entry.
	sd: diff handle
	props: stats written back
	Return: 0: got entry, 1: no more entries */
	int (*diff_next)(ds_sync_diff *sd, struct ds_sync_props *props);

	/** Find duplicate files in snapshot.
	left: snapshot to search
	props: stats written back
	flags: DS_SYNC_DIFF_STEP to enable step mode
	Return: diff handle */
	ds_sync_diff* (*find_dups)(ds_sync_snapshot *left, struct ds_sync_props *props, uint flags);

	/** Free diff. */
	void (*diff_free)(ds_sync_diff *sd);

	/** Filter diff entries into internal list.
	sd: diff handle
	props: filter criteria
	flags: enum DS_SYNC status bits to include
	Return: number of matching entries */
	uint (*view)(ds_sync_diff *sd, struct ds_sync_props *props, uint flags);

	/** Sort filtered entries.
	sd: diff handle
	flags: enum DS_SYNC_SORT
	Return: number of entries */
	uint (*sort)(ds_sync_diff *sd, uint flags);

	/** Get diff entry.
	sd: diff handle
	id: entry identifier, or (void*)(size_t)index if DS_SYNC_INFO_INDEX is set
	flags: enum DS_SYNC_INFO
	dst: output (caller must call ds_sync_diff_entry_destroy())
	Return: 0: success, -1: out of range */
	int (*info)(ds_sync_diff *sd, void *id, uint flags, struct ds_sync_diff_entry *dst);

	/** Update entry status flags.
	sd: diff handle
	id: entry identifier
	mask: bits to modify
	val: new values for masked bits
	Return: resulting status */
	uint (*status)(ds_sync_diff *sd, void *id, uint mask, uint val);

	/** Perform file synchronization.
	sd: diff handle
	diff_entry_ids: entry IDs to sync; must not change between calls
	result: per-entry result; 0: success
	n: number of entries in diff_entry_ids and result
	flags: enum DS_SYNC_SYNC
	on_event: called when async operation completes
	param: opaque, passed to on_event
	Return: >=0: entries completed; -1: waiting for on_event (call again with same args) */
	int (*sync)(ds_sync_diff *sd, void* diff_entry_ids[], int result[], uint n, uint flags, void (*on_event)(void*), void *param);
};
