/** dsynco: CLI: 'sync' command
2026, Simon Zolin */

static int cmd_sync_help(struct args_sync *as)
{
	(void)as;
	help_info_write(FFSTR_Z("\
`sync` command:\n\
\n\
  dsynco sync SOURCE -o TARGET [OPTIONS]\n\
\n\
OPTIONS:\n\
  `-add`    Copy new files\n\
  `-delete` Delete old/removed files\n\
  `-update` Overwrite modified files\n\
  `-move`   Move (moved) files\n\
  `-test`   Dry-run\n\
  `-o` DIR  Output directory (required)\n\
\n\
Examples:\n\
  dsynco sync /d1 -o /d2\n\
  dsynco sync /d1 -o /d2 -a -d\n\
  dsynco sync /d1 -o /d2 -u -m\n\
"));
	return 1;
}

static int sync_done(struct args_sync *as)
{
	struct args *a = (void*)as;
	if (!as->output)
		return 1;
	ffstr_setz(&a->sc.output, as->output);
	a->sc.sync_add = as->add;
	a->sc.sync_del = as->del;
	a->sc.sync_move = as->move;
	a->sc.sync_update = as->update;
	a->sc.test = as->test;
	return 0;
}

#define O(m)  (void*)(ffsize)FF_OFF(struct args_sync, m)
static const struct ffarg cmd_sync[] = {
	{ "-add",      '1',  O(add) },
	{ "-delete",   '1',  O(del) },
	{ "-help",     '0',  cmd_sync_help },
	{ "-move",     '1',  O(move) },
	{ "-o",        '=s', O(output) },
	{ "-test",     '1',  O(test) },
	{ "-update",   '1',  O(update) },
	{ "\0\1",      '+S', arg_input },
	{ "",          '0',  sync_done },
};
#undef O

static struct ffarg_ctx cmd_sync_init(struct args *a)
{
	a->mode = MODE_SYNC;
	return (struct ffarg_ctx){ .scheme = cmd_sync, .obj = &a->sync };
}
