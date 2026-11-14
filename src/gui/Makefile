# dsynco: GUI exe

FFGUI := $(ROOT)/ffgui
WDT := $(ROOT)/winapi-dark-theme
GUI_EXE := dsynco-gui$(DOTEXE)

ifeq "$(OS)" "windows"
	CFLAGS_GUI := -Wno-missing-field-initializers
	LINKFLAGS_GUI := -lshell32 -luxtheme -lcomctl32 -lcomdlg32 -lgdi32 -lole32 -luuid
	FFGUI_OBJ := \
		ffgui-winapi.o \
		ffgui-winapi-loader.o
else
	CFLAGS_GUI := -Wno-free-nonheap-object -Wno-deprecated-declarations \
		`pkg-config --cflags gtk+-3.0`
	LINKFLAGS_GUI := `pkg-config --libs gtk+-3.0` \
		$(LINK_PTHREAD) -lm
	FFGUI_OBJ := \
		ffgui-gtk.o \
		ffgui-gtk-loader.o
endif
ifeq "$(DEBUG_GUI)" "1"
	CFLAGS_GUI += -DFFGUI_DEBUG
endif
CFLAGS_GUI += -I$(FFGUI) -I$(WDT)
CXXFLAGS_GUI := $(CXXFLAGS) $(CFLAGS_GUI)
CFLAGS_GUI := $(CFLAGS) $(CFLAGS_GUI)

%.o: $(FFGUI)/ffgui/gtk/%.c
	$(C) $(CFLAGS_GUI) $< -o $@

%.o: $(FFGUI)/ffgui/winapi/%.c
	$(C) $(CFLAGS_GUI) $< -o $@

gsync.o: $(DS_DIR)/src/gui/gsync.cpp
	$(CXX) $(CXXFLAGS_GUI) $< -o $@

gui-main.o: $(DS_DIR)/src/gui/main.c
	$(C) $(CFLAGS) $< -o $@

$(GUI_EXE): gui-main.o gsync.o \
		$(FFGUI_OBJ) \
		$(EXE_COFF) \
		libdsynco.$(SO)
	$(LINKXX) $+ $(LINKXXFLAGS) $(LINKFLAGS_GUI) $(LINK_RPATH_ORIGIN) -o $@

gui-app:
	$(CP) $(GUI_EXE) \
		$(DS_DIR)/src/gui/dsynco.ui \
		$(APP_DIR)
ifeq "$(OS)" "linux"
	$(CP) $(DS_DIR)/res/dsynco.desktop \
		$(DS_DIR)/res/dsynco.svg \
		$(APP_DIR)
endif
