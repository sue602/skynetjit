# redislib: Redis 8.10.0 algorithm kernels + the luaclib/redis/* facade set.
#
# The including platform makefile must define the shared BASE_CFLAGS / SHARED /
# LUA_LIBS / SKYNET_LINK / SYSTEM_LIBS machinery first, plus:
#   RL_CFLAGS     kernel + facade compile flags (BASE_CFLAGS plus whatever the
#                 platform needs to force its compat headers)
#   OBJ_DIR       object staging dir (Windows: $(OUT)/obj; linux.mk defines it
#                 here because it has no object dir of its own)
#   NB_SKYNET_DEP the skynet library the facades link against
#   NB_RPATH      $(OUT)-relative rpath for the facades (empty on Windows)
# Set REDISLIB=0 to leave the whole set out of the build.
#
# Task 1 deliberately adds no LUA_CLIB entries: the kernel objects are only
# built once a facade .so target asks for them (tasks 2-6), so including this
# file cannot change what the current build produces.

REDISLIB ?= 1
ifneq ($(REDISLIB),0)

RL_SRC := $(INTEGRATION_DIR)/redislib-src
RL_KERNEL := $(RL_SRC)/kernel
RL_FACADE := $(RL_SRC)/facade

# $(OBJ_DIR) may be unset on platforms without an object staging dir yet.
OBJ_DIR ?= $(OUT)/obj
RL_OBJ_DIR := $(OBJ_DIR)/rl

# Kernel objects are shared prerequisites of every facade .so. Without .PRECIOUS
# make treats them as intermediates and deletes them after each .so, which both
# recompiles the whole kernel once per facade and races two facade links under
# the -j used by build.sh / build-linux.sh. The line must follow RL_OBJ_DIR:
# special-target patterns are expanded where they are read.
.PRECIOUS: $(RL_OBJ_DIR)/%.o

RL_KOBJS := $(patsubst $(RL_KERNEL)/%.c,$(RL_OBJ_DIR)/%.o,$(wildcard $(RL_KERNEL)/*.c))

# The kernel is plain C: no Lua headers, no lua54_compat force-include. It does
# need skynet-src on the include path for skynet_malloc.h, which RL_CFLAGS
# already carries.
#
# Two -Wno- flags are kernel-only, so the facade rule keeps the full -Wall:
#   -Wno-pointer-to-int-cast  Windows is LLP64 (long is 32 bits), so upstream's
#     pointer casts warn there: the alignment masks in bitops.c, the pointer
#     terms of dictFingerprint() in dict.c and lpRepr() in listpack.c. All of
#     them either keep the bits they mask (page-aligned allocations) or feed a
#     debug-only fingerprint, i.e. no behaviour depends on the width. Editing
#     them would put non-upstream casts inside verbatim kernel files, which this
#     port forbids; util.c's (long)fp -> (uintptr_t)fp is the documented
#     exception (NOTICE).
#   -Wno-unused-variable      file-scope symbols whose only consumers were the
#     deleted command layer, e.g. hyperloglog.c's invalid_hll_err.
$(RL_OBJ_DIR)/%.o: $(RL_KERNEL)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(RL_CFLAGS) -Wno-pointer-to-int-cast -Wno-unused-variable -c $< -o $@

# rl_common.c is the shared facade object (lua headers come from RL_CFLAGS via
# BASE_CFLAGS); give it its own rule so it does not fall into the kernel one.
$(RL_OBJ_DIR)/rl_common.o: $(RL_FACADE)/rl_common.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(RL_CFLAGS) -I$(RL_KERNEL) -I$(RL_FACADE) -c $< -o $@

# Facade modules land in the redis/ sub namespace: luaclib/redis/{bitmap,geo,
# hll,sortedset}.so, required as "redis.bitmap" and friends (same precedent as
# NetBull's luaclib/skiplist/c.so).
$(LUA_CLIB_DIR)/redis:
	mkdir -p $@

$(LUA_CLIB_DIR)/redis/%.so: $(RL_FACADE)/%.c $(RL_OBJ_DIR)/rl_common.o $(RL_KOBJS) \
	| $(LUA_CLIB_DIR) $(LUA_CLIB_DIR)/redis $(NB_SKYNET_DEP)
	$(CC) $(RL_CFLAGS) $(SHARED) -I$(RL_KERNEL) -I$(RL_FACADE) -o $@ $< \
		$(RL_OBJ_DIR)/rl_common.o $(RL_KOBJS) \
		$(SKYNET_LINK) $(LUA_LIBS) $(SYSTEM_LIBS) $(NB_RPATH)

# LUA_CLIB entries are appended by the facade tasks themselves, one per module
# as it becomes available:
LUA_CLIB += redis/bitmap redis/hll
#   LUA_CLIB += redis/hll        (Task 3)
#   LUA_CLIB += redis/sortedset  (Task 4)
#   LUA_CLIB += redis/geo        (Task 5)

endif
