/** dsynco: CLI: 'diff' command
2026, Simon Zolin */

static int cmd_diff_help(struct args_diff *ad)
{
	(void)ad;
	help_info_write(FFSTR_Z("\
`diff` command:\n\
\n\
  dsynco diff SOURCE -o TARGET [OPTIONS]\n\
\n\
OPTIONS:\n\
  `-add`    Add (new files)\n\
  `-delete` Delete (old/removed files)\n\
  `-update` Update (modified files)\n\
  `-move`   Move (moved files)\n\
  `-o` DIR  Output directory (required)\n\
\n\
Examples:\n\
  dsynco diff /d1 -o /d2\n\
  dsynco diff /d1 -o /d2 -a\n\
  dsynco diff /d1 -o /d2 -u -m\n\
"));
	return 1;
}

static int diff_done(struct args_diff *ad)
{
	struct args *a = (void*)ad;
	if (!ad->output)
		return 1;
	ffstr_setz(&a->sc.output, ad->output);

	uint f = 0;
	if (ad->add) f |= DS_SYNC_LEFT;
	if (ad->del) f |= DS_SYNC_RIGHT;
	if (ad->move) f |= DS_SYNC_MOVE;
	if (ad->update) f |= DS_SYNC_NEQ;
	if (!f)
		f = DS_SYNC_LEFT | DS_SYNC_RIGHT | DS_SYNC_NEQ | DS_SYNC_MOVE;
	a->sc.diff_flags = f;
	return 0;
}

#define O(m)  (void*)(ffsize)FF_OFF(struct args_diff, m)
static const struct ffarg cmd_diff[] = {
	{ "-add",      '1',  O(add) },
	{ "-delete",   '1',  O(del) },
	{ "-help",     '0',  cmd_diff_help },
	{ "-move",     '1',  O(move) },
	{ "-o",        '=s', O(output) },
	{ "-update",   '1',  O(update) },
	{ "\0\1",      '+S', arg_input },
	{ "",          '0',  diff_done },
};
#undef O

static struct ffarg_ctx cmd_diff_init(struct args *a)
{
	a->mode = MODE_DIFF;
	return (struct ffarg_ctx){ .scheme = cmd_diff, .obj = &a->diff };
}
