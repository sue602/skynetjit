CC ?= gcc
AR ?= ar
RANLIB ?= ranlib
PKG_CONFIG ?= pkg-config

SKYNET_DIR ?= .
LUAJIT_DIR ?= ../luajit2
OUT ?= ../out
INTEGRATION_DIR ?= ../..

SOCKET_BACKEND ?= epoll

COMPAT_LUA := $(INTEGRATION_DIR)/compat/luajit
LUAJIT_SRC := $(LUAJIT_DIR)/src

OBJ_DIR := $(OUT)/obj
CSERVICE_DIR := $(OUT)/cservice
LUA_CLIB_DIR := $(OUT)/luaclib

BASE_CFLAGS := -g -O2 -Wall -std=gnu99 -fno-strict-aliasing -fPIC \
	-I$(LUAJIT_SRC) -I$(SKYNET_DIR)/skynet-src
BASE_CFLAGS += -Wno-unused-function -Wno-unused-parameter
LUA_FORCE := -include $(COMPAT_LUA)/lua54_compat.h
SKYNET_CFLAGS := $(BASE_CFLAGS) $(LUA_FORCE)
LUA51_CFLAGS := $(BASE_CFLAGS)

ifeq ($(SOCKET_BACKEND),uring)
BACKEND_CFLAGS := -DSKYNETJIT_USE_IO_URING -I$(INTEGRATION_DIR)/compat/linux
BACKEND_LIBS := $(shell $(PKG_CONFIG) --libs liburing 2>/dev/null)
else ifeq ($(SOCKET_BACKEND),epoll)
BACKEND_CFLAGS :=
BACKEND_LIBS :=
else
$(error SOCKET_BACKEND must be epoll or uring)
endif

SHARED := -shared
LUA_LIBS := -L$(LUAJIT_SRC) -lluajit-5.1
SKYNET_LINK := -L$(OUT) -lskynet
SYSTEM_LIBS := -pthread -lm -ldl
RPATH := -Wl,-rpath,'$$ORIGIN'

SKYNET_SO := $(OUT)/libskynet.so
SKYNET_BIN := $(OUT)/skynet

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

core: $(SKYNET_SO)

modules: core $(foreach name,$(CSERVICE),$(CSERVICE_DIR)/$(name).so) \
	$(foreach name,$(LUA_CLIB),$(LUA_CLIB_DIR)/$(name).so)

tools: $(SKYNET_BIN)

print-config:
	@echo CC=$(CC)
	@echo SKYNET_DIR=$(SKYNET_DIR)
	@echo LUAJIT_DIR=$(LUAJIT_DIR)
	@echo OUT=$(OUT)
	@echo SOCKET_BACKEND=$(SOCKET_BACKEND)

$(OUT) $(OBJ_DIR) $(CSERVICE_DIR) $(LUA_CLIB_DIR):
	mkdir -p $@

$(SKYNET_SO): $(addprefix $(SKYNET_DIR)/skynet-src/,$(SKYNET_SRC)) \
	$(LUAJIT_SRC)/libluajit-5.1.so | $(OUT)
	$(CC) $(SKYNET_CFLAGS) $(BACKEND_CFLAGS) $(SHARED) -o $@ \
		$(addprefix $(SKYNET_DIR)/skynet-src/,$(SKYNET_SRC)) \
		$(LUA_LIBS) $(SYSTEM_LIBS) $(BACKEND_LIBS) -DNOUSE_JEMALLOC

$(SKYNET_BIN): $(SKYNET_DIR)/skynet-src/skynet_main.c $(SKYNET_SO) | $(OUT)
	$(CC) $(SKYNET_CFLAGS) $(BACKEND_CFLAGS) -o $@ $< \
		$(SKYNET_LINK) $(LUA_LIBS) $(SYSTEM_LIBS) $(BACKEND_LIBS) \
		$(RPATH) -Wl,-E -DNOUSE_JEMALLOC

define CSERVICE_template
$(CSERVICE_DIR)/$(1).so: $(SKYNET_DIR)/service-src/service_$(1).c \
	$(SKYNET_SO) | $(CSERVICE_DIR)
	$$(CC) $$(SKYNET_CFLAGS) $$(BACKEND_CFLAGS) $$(SHARED) -o $$@ $$< \
		$$(SKYNET_LINK) $$(LUA_LIBS) $$(SYSTEM_LIBS) $$(BACKEND_LIBS) \
		-Wl,-rpath,'$$$$ORIGIN/..'
endef
$(foreach name,$(CSERVICE),$(eval $(call CSERVICE_template,$(name))))

$(LUA_CLIB_DIR)/skynet.so: \
	$(addprefix $(SKYNET_DIR)/lualib-src/,$(LUA_CLIB_SKYNET)) \
	$(COMPAT_LUA)/sharetable_stub.c $(COMPAT_LUA)/sharetable_bridge.c \
	$(SKYNET_SO) | $(LUA_CLIB_DIR)
	$(CC) $(SKYNET_CFLAGS) $(BACKEND_CFLAGS) $(SHARED) -o $@ \
		$(addprefix $(SKYNET_DIR)/lualib-src/,$(LUA_CLIB_SKYNET)) \
		$(COMPAT_LUA)/sharetable_stub.c $(COMPAT_LUA)/sharetable_bridge.c \
		-I$(SKYNET_DIR)/service-src -I$(SKYNET_DIR)/lualib-src \
		$(SKYNET_LINK) $(LUA_LIBS) $(SYSTEM_LIBS) $(BACKEND_LIBS) \
		-Wl,-rpath,'$$ORIGIN/..'

$(LUA_CLIB_DIR)/client.so: $(SKYNET_DIR)/lualib-src/lua-clientsocket.c \
	$(SKYNET_DIR)/lualib-src/lua-crypt.c $(SKYNET_DIR)/lualib-src/lsha1.c \
	$(SKYNET_SO) | $(LUA_CLIB_DIR)
	$(CC) $(SKYNET_CFLAGS) $(BACKEND_CFLAGS) $(SHARED) -o $@ $^ \
		$(LUA_LIBS) $(SYSTEM_LIBS) $(BACKEND_LIBS) -Wl,-rpath,'$$ORIGIN/..'

$(LUA_CLIB_DIR)/bson.so: $(SKYNET_DIR)/lualib-src/lua-bson.c | $(LUA_CLIB_DIR)
	$(CC) $(SKYNET_CFLAGS) $(SHARED) -o $@ $< $(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/md5.so: $(SKYNET_DIR)/3rd/lua-md5/md5.c \
	$(SKYNET_DIR)/3rd/lua-md5/md5lib.c \
	$(SKYNET_DIR)/3rd/lua-md5/compat-5.2.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) $(SHARED) -I$(SKYNET_DIR)/3rd/lua-md5 \
		-o $@ $^ $(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/sproto.so: $(SKYNET_DIR)/lualib-src/sproto/sproto.c \
	$(SKYNET_DIR)/lualib-src/sproto/lsproto.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) -include $(COMPAT_LUA)/sproto_compat.h \
		$(SHARED) -I$(SKYNET_DIR)/lualib-src/sproto -o $@ $^ \
		$(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/lpeg.so: $(SKYNET_DIR)/3rd/lpeg/lpcap.c \
	$(SKYNET_DIR)/3rd/lpeg/lpcode.c $(SKYNET_DIR)/3rd/lpeg/lpprint.c \
	$(SKYNET_DIR)/3rd/lpeg/lptree.c $(SKYNET_DIR)/3rd/lpeg/lpvm.c \
	$(SKYNET_DIR)/3rd/lpeg/lpcset.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) $(SHARED) -I$(SKYNET_DIR)/3rd/lpeg \
		-o $@ $^ $(LUA_LIBS) $(SYSTEM_LIBS)

clean:
	rm -rf $(OUT)
