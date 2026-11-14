# dsynco: CLI exe

CLI_EXE := dsynco$(DOTEXE)

%.o: $(DS_DIR)/src/cli/%.c
	$(C) $(CFLAGS) $< -o $@

$(CLI_EXE): main.o \
		$(EXE_COFF) \
		libdsynco.$(SO)
	$(LINK) $+ $(LINKFLAGS) $(LINK_RPATH_ORIGIN) -o $@
