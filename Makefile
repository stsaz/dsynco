# dsynco build rules

ROOT := ..
DS_DIR := $(ROOT)/dsynco
FFBASE_DIR := $(ROOT)/ffbase
FFSYS_DIR := $(ROOT)/ffsys

include $(FFBASE_DIR)/conf.mk

CFLAGS += -MMD -MP \
	-I$(DS_DIR)/src -I$(FFSYS_DIR) -I$(FFBASE_DIR) \
	-DFFBASE_HAVE_FFERR_STR -DFFBASE_MEM_ASSERT \
	-Wall -Wextra -Wno-unused-parameter -Wno-multichar -Wno-typedef-redefinition \
	-fPIC \
	-g
ifeq "$(DEBUG)" "1"
	CFLAGS += -O0 -DFF_DEBUG -Werror
else
	CFLAGS += -O3 -fno-strict-aliasing -fvisibility=hidden
endif
ifeq "$(ASAN)" "1"
	CFLAGS += -fsanitize=address
	LINKFLAGS += -fsanitize=address
endif
CXXFLAGS := -std=gnu++98 $(CFLAGS)
CFLAGS := -std=c99 $(CFLAGS)
ifeq "$(OS)" "windows"
	LINKFLAGS += -lws2_32
endif
LINKXXFLAGS := $(LINKFLAGS) -static-libgcc
ifeq "$(OS)" "windows"
	# remove runtime dependency on both libstdc++-6.dll and libwinpthread-1.dll
	LINKXXFLAGS += -Wl,-Bstatic
else
	LINKXXFLAGS += -static-libstdc++
endif
LINK_DL :=
ifeq "$(OS)" "linux"
	LINK_DL := -ldl
endif

default: build
ifneq "$(DEBUG)" "1"
	$(SUBMAKE) strip-debug
endif
	$(SUBMAKE) app

_:=
ifeq "$(OS)" "windows"
	EXE_COFF := exe.coff
endif
exe.coff: $(DS_DIR)/res/exe.rc \
		$(DS_DIR)/res/dsynco.ico \
		$(DS_DIR)/res/exe.manifest
	$(WINDRES) $< $@

-include $(wildcard *.d)

include $(DS_DIR)/src/core/Makefile
include $(DS_DIR)/src/cli/Makefile
include $(DS_DIR)/src/gui/Makefile

build: $(CLI_EXE) $(GUI_EXE)

clean:
	$(RM) *.o


%.$(SO): %.o
	$(LINK) -shared $+ $(LINKFLAGS) -o $@

test: test.o
	$(LINK) $+ $(LINKFLAGS) -o $@


# copy files to app directory
APP_DIR := dsynco-0
app:
	$(MKDIR) $(APP_DIR)
	$(CP) $(CLI_EXE) libdsynco.$(SO) \
		$(DS_DIR)/README.md \
		$(APP_DIR)
	$(SUBMAKE) gui-app


# package
PKG_VER := $(DS_VER)
PKG_ARCH := x86_64
ifeq "$(OS)" "windows"
	PKG_ARCH := x64
endif
PKG_PACKER := tar -c --owner=0 --group=0 --numeric-owner -v --zstd -f
PKG_EXT := tar.zst
ifeq "$(OS)" "windows"
	PKG_PACKER := zip -r -v
	PKG_EXT := zip
endif

PKG_NAME := dsynco-$(PKG_VER)-$(OS)-$(PKG_ARCH).$(PKG_EXT)
$(PKG_NAME): $(APP_DIR)
	$(PKG_PACKER) $@ $<
package: $(PKG_NAME)

release: default
	$(SUBMAKE) package
