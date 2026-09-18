# Vendored NetBull luaclib set. The including platform makefile must define
# NB_CURL_CFLAGS, NB_CURL_LIBS, NB_ZLIB_CFLAGS, NB_ZLIB_LIBS, NB_SKYNET_DEP
# and the shared BASE_CFLAGS/SHARED/LUA_LIBS/SYSTEM_LIBS machinery first.

NETBULL_SRC := $(INTEGRATION_DIR)/luaclib-src
LUA_CLIB += cjson lfs pb sqlite3 luacurl zlib skiplist/c

$(LUA_CLIB_DIR)/skiplist:
	mkdir -p $@

$(LUA_CLIB_DIR)/cjson.so: $(NETBULL_SRC)/cjson/lua_cjson.c \
	$(NETBULL_SRC)/cjson/strbuf.c $(NETBULL_SRC)/cjson/fpconv.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) $(SHARED) -I$(NETBULL_SRC)/cjson \
		-o $@ $^ $(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/lfs.so: $(NETBULL_SRC)/lfs/lfs.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) $(SHARED) -I$(NETBULL_SRC)/lfs \
		-o $@ $< $(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/pb.so: $(NETBULL_SRC)/pb/pb.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) $(SHARED) -I$(NETBULL_SRC)/pb \
		-o $@ $< $(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/sqlite3.so: $(NETBULL_SRC)/sqlite3/ls_sqlite3.c \
	$(NETBULL_SRC)/sqlite3/luasql.c $(NETBULL_SRC)/sqlite3/sqlite3.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) $(SHARED) -I$(NETBULL_SRC)/sqlite3 \
		-o $@ $^ $(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/luacurl.so: $(NETBULL_SRC)/curl/luacurl.c \
	$(NETBULL_SRC)/curl/multi.c $(NETBULL_SRC)/curl/constants.c | $(LUA_CLIB_DIR)
	$(CC) $(LUA51_CFLAGS) $(NB_CURL_CFLAGS) $(SHARED) -I$(NETBULL_SRC)/curl \
		-o $@ $^ $(NB_CURL_LIBS) $(LUA_LIBS) $(SYSTEM_LIBS)

$(LUA_CLIB_DIR)/zlib.so: $(NETBULL_SRC)/zlib/lua_zlib.c | $(LUA_CLIB_DIR) $(NB_SKYNET_DEP)
	$(CC) $(LUA51_CFLAGS) $(NB_ZLIB_CFLAGS) $(SHARED) \
		-o $@ $< $(NB_ZLIB_LIBS) $(SKYNET_LINK) $(LUA_LIBS) $(SYSTEM_LIBS) $(NB_RPATH)

$(LUA_CLIB_DIR)/skiplist/c.so: $(NETBULL_SRC)/zset/lua-skiplist.c \
	$(NETBULL_SRC)/zset/skiplist.c | $(LUA_CLIB_DIR)/skiplist
	$(CC) $(LUA51_CFLAGS) $(SHARED) -I$(NETBULL_SRC)/zset \
		-o $@ $^ $(LUA_LIBS) $(SYSTEM_LIBS)
