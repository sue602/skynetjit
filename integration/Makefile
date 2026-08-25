CC ?= x86_64-w64-mingw32-gcc
AR ?= x86_64-w64-mingw32-ar
RANLIB ?= x86_64-w64-mingw32-ranlib
WINDOWS_FD_SETSIZE ?= 8192

SKYNET_DIR ?= .
LUAJIT_DIR ?= ../luajit2
OUT ?= ../out
INTEGRATION_DIR ?= ../..

COMPAT_WIN := $(INTEGRATION_DIR)/compat/win64
COMPAT_LUA := $(INTEGRATION_DIR)/compat/luajit
COMPAT_UPSTREAM := $(SKYNET_DIR)/3rd/compat-mingw
LUAJIT_SRC := $(LUAJIT_DIR)/src

OBJ_DIR := $(OUT)/obj
CSERVICE_DIR := $(OUT)/cservice
LUA_CLIB_DIR := $(OUT)/luaclib

BASE_CFLAGS := -g -O2 -Wall -std=gnu99 -fno-strict-aliasing \
	-DFD_SETSIZE=$(WINDOWS_FD_SETSIZE) -D_WIN32_WINNT=0x0601 \
	-I$(COMPAT_WIN) -I$(COMPAT_UPSTREAM) -I$(LUAJIT_SRC) \
	-I$(SKYNET_DIR)/skynet-src
BASE_CFLAGS += -Wno-unused-function -Wno-unused-parameter \
	-Wno-deprecated-declarations

WIN_FORCE := -include $(COMPAT_WIN)/compat.h
LUA_FORCE := -include $(COMPAT_LUA)/lua54_compat.h
SKYNET_CFLAGS := $(BASE_CFLAGS) $(WIN_FORCE) $(LUA_FORCE)
LUA51_CFLAGS := $(BASE_CFLAGS) $(WIN_FORCE)

SHARED := -shared -Wl,--export-all-symbols,--enable-auto-import
LUA_LIBS := -L$(LUAJIT_SRC) -lluajit-5.1
SKYNET_LINK := -L$(OUT) -lskynet
SYSTEM_LIBS := -static-libgcc -Wl,-Bstatic -lpthread -Wl,-Bdynamic \
	-lm -lws2_32 -lgdi32

COMPAT_LIB := $(OUT)/libcompat.a
SKYNET_DLL := $(OUT)/skynet.dll
SKYNET_EXE := $(OUT)/skynet.exe

CSERVICE := snlua logger gate harbor
LUA_CLIB := skynet client bson md5 sproto lpeg

SKYNET_SRC := skynet_handle.c skynet_module.c skynet_mq.c \
	skynet_server.c skynet_start.c skynet_timer.c skynet_error.c \
	skynet_harbor.c skynet_env.c skynet_monitor.c skynet_socket.c \
	socket_server.c mem_info.c malloc_hook.c skynet_daemon.c skynet_log.c

LUA_CLIB_SKYNET := lua-skynet.c lua-seri.c lua-socket.c lua-mongo.c \
	lua-netpack.c lua-memory.c lua-multicast.c lua-cluster.c lua-crypt.c \
	lsha1.c lua-sharedata.c lua-stm.c lua-debugchannel.c lua-datasheet.c

.PHONY: all core modules tools clean print-config

all: core modules tools

core: $(COMPAT_LIB) $(SKYNET_DLL)

modules: core $(foreach name,$(CSERVICE),$(CSERVICE_DIR)/$(name).so) \
	$(foreach name,$(LUA_CLIB),$(LUA_CLIB_DIR)/$(name).so)

tools: $(SKYNET_EXE)

print-config:
	@echo CC=$(CC)
	@echo SKYNET_DIR=$(SKYNET_DIR)
	@echo LUAJIT_DIR=$(LUAJIT_DIR)
	@echo OUT=$(OUT)
	@echo WINDOWS_FD_SETSIZE=$(WINDOWS_FD_SETSIZE)

$(OUT) $(OBJ_DIR) $(CSERVICE_DIR) $(LUA_CLIB_DIR):
	mkdir -p $@

$(OBJ_DIR)/win_compat.o: $(COMPAT_WIN)/win_compat.c | $(OBJ_DIR)
	$(CC) $(BASE_CFLAGS) -c $< -o $@

$(OBJ_DIR)/dlfcn.o: $(COMPAT_UPSTREAM)/dlfcn.c | $(OBJ_DIR)
	$(CC) $(BASE_CFLAGS) -I$(COMPAT_UPSTREAM) -c $< -o $@

$(OBJ_DIR)/wepoll.o: $(COMPAT_UPSTREAM)/wepoll.c | $(OBJ_DIR)
	$(CC) -g -O2 -Wall -std=gnu99 \
		-DFD_SETSIZE=$(WINDOWS_FD_SETSIZE) \
		-D_WIN32_WINNT=0x0601 -I$(COMPAT_UPSTREAM) -c $< -o $@

$(COMPAT_LIB): $(OBJ_DIR)/win_compat.o $(OBJ_DIR)/dlfcn.o \
	$(OBJ_DIR)/wepoll.o | $(OUT)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(SKYNET_DLL): $(addprefix $(SKYNET_DIR)/skynet-src/,$(SKYNET_SRC)) \
	$(COMPAT_LIB) $(LUAJIT_SRC)/lua51.dll | $(OUT)
	$(CC) $(SKYNET_CFLAGS) $(SHARED) -o $@ \
		$(addprefix $(SKYNET_DIR)/skynet-src/,$(SKYNET_SRC)) \
		$(COMPAT_LIB) $(LUA_LIBS) $(SYSTEM_LIBS) \
		-DNOUSE_JEMALLOC \
		-Wl,--out-implib,$(OUT)/libskynet.a

$(SKYNET_EXE): $(SKYNET_DIR)/skynet-src/skynet_main.c $(SKYNET_DLL) \
	$(COMPAT_LIB) | $(OUT)
	$(CC) $(SKYNET_CFLAGS) -o $@ $< $(COMPAT_LIB) $(SKYNET_LINK) \
		$(LUA_LIBS) $(SYSTEM_LIBS) -DNOUSE_JEMALLOC

define CSERVICE_template
$(CSERVICE_DIR)/$(1).so: $(SKYNET_DIR)/service-src/service_$(1).c \
	$(SKYNET_DLL) $(COMPAT_LIB) | $(CSERVICE_DIR)
	$$(CC) $$(SKYNET_CFLAGS) $$(SHARED) -o $$@ $$< $$(COMPAT_LIB) \
		$$(SKYNET_LINK) $$(LUA_LIBS) $$(SYSTEM_LIBS)
endef
$(foreach name,$(CSERVICE),$(eval $(call CSERVICE_template,$(name))))

$(LUA_CLIB_DIR)/skynet.so: \
	$(addprefix $(SKYNET_DIR)/lualib-src/,$(LUA_CLIB_SKYNET)) \
	$(COMPAT_LUA)/sharetable_stub.c $(COMPAT_LUA)/sharetable_bridge.c \
	$(SKYNET_DLL) $(COMPAT_LIB) \
	| $(LUA_CLIB_DIR)
	$(CC) $(SKYNET_CFLAGS) $(SHARED) -o $@ \
		$(addprefix $(SKYNET_DIR)/lualib-src/,$(LUA_CLIB_SKYNET)) \
		$(COMPAT_LUA)/sharetable_stub.c \
		$(COMPAT_LUA)/sharetable_bridge.c \
		-I$(SKYNET_DIR)/service-src -I$(SKYNET_DIR)/lualib-src \
		$(COMPAT_LIB) $(SKYNET_LINK) $(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/client.so: $(SKYNET_DIR)/lualib-src/lua-clientsocket.c \
	$(SKYNET_DIR)/lualib-src/lua-crypt.c \
	$(SKYNET_DIR)/lualib-src/lsha1.c $(COMPAT_LIB) | $(LUA_CLIB_DIR)
	$(CC) $(SKYNET_CFLAGS) $(SHARED) -o $@ $^ \
		$(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/bson.so: $(SKYNET_DIR)/lualib-src/lua-bson.c \
	| $(LUA_CLIB_DIR)
	$(CC) $(SKYNET_CFLAGS) $(SHARED) -o $@ $< \
		$(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/md5.so: $(SKYNET_DIR)/3rd/lua-md5/md5.c \
	$(SKYNET_DIR)/3rd/lua-md5/md5lib.c \
	$(SKYNET_DIR)/3rd/lua-md5/compat-5.2.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) $(SHARED) -I$(SKYNET_DIR)/3rd/lua-md5 \
		-o $@ $^ $(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/sproto.so: $(SKYNET_DIR)/lualib-src/sproto/sproto.c \
	$(SKYNET_DIR)/lualib-src/sproto/lsproto.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) -include $(COMPAT_LUA)/sproto_compat.h \
		$(SHARED) -I$(SKYNET_DIR)/lualib-src/sproto \
		-o $@ $^ $(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/lpeg.so: $(SKYNET_DIR)/3rd/lpeg/lpcap.c \
	$(SKYNET_DIR)/3rd/lpeg/lpcode.c $(SKYNET_DIR)/3rd/lpeg/lpprint.c \
	$(SKYNET_DIR)/3rd/lpeg/lptree.c $(SKYNET_DIR)/3rd/lpeg/lpvm.c \
	$(SKYNET_DIR)/3rd/lpeg/lpcset.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) $(SHARED) -I$(SKYNET_DIR)/3rd/lpeg \
		-o $@ $^ $(LUA_LIBS) $(SYSTEM_LIBS)

clean:
	rm -rf $(OUT)
