# dsynco: core

core.o: CFLAGS += -DDS_VER=\"$(DS_VER)\"
%.o: $(DS_DIR)/src/core/%.c
	$(C) $(CFLAGS) $< -o $@

%.o: $(DS_DIR)/src/fs/%.c
	$(C) $(CFLAGS) $< -o $@

%.o: $(DS_DIR)/src/sync/%.c
	$(C) $(CFLAGS) $< -o $@

libdsynco.$(SO): core.o \
		file.o \
		sync.o \
		copy.o \
		trash.o
	$(LINK) -shared $+ $(LINKFLAGS) $(LINK_DL) -o $@
