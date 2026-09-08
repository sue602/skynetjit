CC ?= gcc

SKYNET_DIR ?= .
LUAJIT_DIR ?= ../luajit2
OUT ?= ../out
INTEGRATION_DIR ?= ../..

COMPAT_LUA := $(INTEGRATION_DIR)/compat/luajit
LUAJIT_SRC := $(LUAJIT_DIR)/src

CSERVICE_DIR := $(OUT)/cservice
LUA_CLIB_DIR := $(OUT)/luaclib

BASE_CFLAGS := -g -O2 -Wall -std=gnu99 -fno-strict-aliasing -fPIC \
	-I$(LUAJIT_SRC) -I$(SKYNET_DIR)/skynet-src
BASE_CFLAGS += -Wno-unused-function -Wno-unused-parameter
LUA_FORCE := -include $(COMPAT_LUA)/lua54_compat.h
SKYNET_CFLAGS := $(BASE_CFLAGS) $(LUA_FORCE)
LUA51_CFLAGS := $(BASE_CFLAGS)

SHARED := -shared
LUA_LIBS := -L$(LUAJIT_SRC) -lluajit
SKYNET_LINK := -L$(OUT) -lskynet
SYSTEM_LIBS := -pthread -lm -ldl
RPATH := -Wl,-rpath,'$$ORIGIN'

SKYNET_SO := $(OUT)/libskynet.so
SKYNET_BIN := $(OUT)/skynet

include $(INTEGRATION_DIR)/integration/sources.mk

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
	@echo Linux socket backend: epoll

$(OUT) $(CSERVICE_DIR) $(LUA_CLIB_DIR):
	mkdir -p $@

$(SKYNET_SO): $(addprefix $(SKYNET_DIR)/skynet-src/,$(SKYNET_SRC)) \
	$(LUAJIT_SRC)/libluajit.so | $(OUT)
	$(CC) $(SKYNET_CFLAGS) $(SHARED) -o $@ \
		$(addprefix $(SKYNET_DIR)/skynet-src/,$(SKYNET_SRC)) \
		$(LUA_LIBS) $(SYSTEM_LIBS) -DNOUSE_JEMALLOC

$(SKYNET_BIN): $(SKYNET_DIR)/skynet-src/skynet_main.c $(SKYNET_SO) | $(OUT)
	$(CC) $(SKYNET_CFLAGS) -o $@ $< \
		$(SKYNET_LINK) $(LUA_LIBS) $(SYSTEM_LIBS) \
		$(RPATH) -Wl,-E -DNOUSE_JEMALLOC

define CSERVICE_template
$(CSERVICE_DIR)/$(1).so: $(SKYNET_DIR)/service-src/service_$(1).c \
	$(SKYNET_SO) | $(CSERVICE_DIR)
	$$(CC) $$(SKYNET_CFLAGS) $$(SHARED) -o $$@ $$< \
		$$(SKYNET_LINK) $$(LUA_LIBS) $$(SYSTEM_LIBS) \
		-Wl,-rpath,'$$$$ORIGIN/..'
endef
$(foreach name,$(CSERVICE),$(eval $(call CSERVICE_template,$(name))))

$(LUA_CLIB_DIR)/skynet.so: \
	$(addprefix $(SKYNET_DIR)/lualib-src/,$(LUA_CLIB_SKYNET)) \
	$(COMPAT_LUA)/sharetable_stub.c $(COMPAT_LUA)/sharetable_bridge.c \
	$(SKYNET_SO) | $(LUA_CLIB_DIR)
	$(CC) $(SKYNET_CFLAGS) $(SHARED) -o $@ \
		$(addprefix $(SKYNET_DIR)/lualib-src/,$(LUA_CLIB_SKYNET)) \
		$(COMPAT_LUA)/sharetable_stub.c $(COMPAT_LUA)/sharetable_bridge.c \
		-I$(SKYNET_DIR)/service-src -I$(SKYNET_DIR)/lualib-src \
		$(SKYNET_LINK) $(LUA_LIBS) $(SYSTEM_LIBS) \
		-Wl,-rpath,'$$ORIGIN/..'

$(LUA_CLIB_DIR)/client.so: $(SKYNET_DIR)/lualib-src/lua-clientsocket.c \
	$(SKYNET_DIR)/lualib-src/lua-crypt.c $(SKYNET_DIR)/lualib-src/lsha1.c \
	$(SKYNET_SO) | $(LUA_CLIB_DIR)
	$(CC) $(SKYNET_CFLAGS) $(SHARED) -o $@ $^ \
		$(LUA_LIBS) $(SYSTEM_LIBS) -Wl,-rpath,'$$ORIGIN/..'

$(LUA_CLIB_DIR)/bson.so: $(SKYNET_DIR)/lualib-src/lua-bson.c | $(LUA_CLIB_DIR)
	$(CC) $(SKYNET_CFLAGS) $(SHARED) -o $@ $< $(LUA_LIBS) $(SYSTEM_LIBS) \
		-Wl,-rpath,'$$ORIGIN/..'

$(LUA_CLIB_DIR)/md5.so: $(SKYNET_DIR)/3rd/lua-md5/md5.c \
	$(SKYNET_DIR)/3rd/lua-md5/md5lib.c \
	$(SKYNET_DIR)/3rd/lua-md5/compat-5.2.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) $(SHARED) -I$(SKYNET_DIR)/3rd/lua-md5 \
		-o $@ $^ $(LUA_LIBS) $(SYSTEM_LIBS) -Wl,-rpath,'$$ORIGIN/..'

$(LUA_CLIB_DIR)/sproto.so: $(SKYNET_DIR)/lualib-src/sproto/sproto.c \
	$(SKYNET_DIR)/lualib-src/sproto/lsproto.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) -include $(COMPAT_LUA)/sproto_compat.h \
		$(SHARED) -I$(SKYNET_DIR)/lualib-src/sproto -o $@ $^ \
		$(LUA_LIBS) $(SYSTEM_LIBS) -Wl,-rpath,'$$ORIGIN/..'

$(LUA_CLIB_DIR)/lpeg.so: $(SKYNET_DIR)/3rd/lpeg/lpcap.c \
	$(SKYNET_DIR)/3rd/lpeg/lpcode.c $(SKYNET_DIR)/3rd/lpeg/lpprint.c \
	$(SKYNET_DIR)/3rd/lpeg/lptree.c $(SKYNET_DIR)/3rd/lpeg/lpvm.c \
	$(SKYNET_DIR)/3rd/lpeg/lpcset.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) $(SHARED) -I$(SKYNET_DIR)/3rd/lpeg \
		-o $@ $^ $(LUA_LIBS) $(SYSTEM_LIBS) -Wl,-rpath,'$$ORIGIN/..'

clean:
	rm -rf $(OUT)
