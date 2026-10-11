# redislib: Redis 8.10.0 algorithm kernels + the luaclib/redis/* facade set.
#
# The including platform makefile must define the shared BASE_CFLAGS / SHARED /
# LUA_LIBS / SKYNET_LINK / SYSTEM_LIBS machinery first, plus:
#   RL_CFLAGS         kernel + facade compile flags (BASE_CFLAGS plus whatever the
#                     platform needs to force its compat headers)
#   OBJ_DIR           object staging dir (Windows: $(OUT)/obj; linux.mk defines it
#                     here because it has no object dir of its own)
#   NB_SKYNET_DEP     the skynet library the facades link against
#   RL_CORE_SO        the rlcore shared library to build and link against
#   RL_CORE_IMPLIB    extra linker flags producing what -l<rlcore> resolves to
#                     (MinGW: --out-implib; empty on ELF platforms)
#   RL_CORE_LINK      how a facade asks for rlcore (-L$(OUT) -lrlcore)
#   RL_CORE_RPATH     rpath rlcore uses to find its own dependencies
#   RL_RPATH          rpath a luaclib/redis/*.so uses to find rlcore
# Set REDISLIB=0 to leave the whole set out of the build.

REDISLIB ?= 1
ifneq ($(REDISLIB),0)

RL_SRC := $(INTEGRATION_DIR)/redislib-src
RL_KERNEL := $(RL_SRC)/kernel
RL_FACADE := $(RL_SRC)/facade

# $(OBJ_DIR) may be unset on platforms without an object staging dir yet.
OBJ_DIR ?= $(OUT)/obj
RL_OBJ_DIR := $(OBJ_DIR)/rl

# Kernel objects are shared prerequisites of the rlcore library. Without
# .PRECIOUS make treats them as intermediates and deletes them as soon as
# rlcore is linked, which recompiles the whole kernel on every later target in
# the same run. The line must follow RL_OBJ_DIR: special-target patterns are
# expanded where they are read.
.PRECIOUS: $(RL_OBJ_DIR)/%.o

RL_KOBJS := $(patsubst $(RL_KERNEL)/%.c,$(RL_OBJ_DIR)/%.o,$(wildcard $(RL_KERNEL)/*.c))

# rlcore -- the kernels as one shared library instead of one copy per module.
#
# Every facade .so used to link all of $(RL_KOBJS) straight into itself: about
# 1 MB of kernel shipped four times, with four copies of `server`, four dict
# siphash seeds, and four copies of the Windows stand-in for random() -- which
# each module started from the same hardcoded constant, so two modules drew the
# same sequence. One library is loaded once and shared, and the facades import
# their kernel calls from it.
#
# Only functions cross that boundary. A facade must not take the address of
# data defined here: on Windows that is resolved by the linker's auto-import
# rewriting instead of by the import table, so the three globals facades used
# to read directly became accessors in redislib_port.c (NOTICE).
#
# rlcore needs no Lua: the kernels are plain C. $(SKYNET_LINK) is there for the
# read()/write() calls of util.c's dead file helpers, which the include path
# resolves to skynet's socket shims -- so skynet.dll / libskynet.so has to be
# linked before this library, hence $(NB_SKYNET_DEP) as an order-only source.
$(RL_CORE_SO): $(RL_KOBJS) | $(OUT) $(NB_SKYNET_DEP)
	$(CC) $(RL_CFLAGS) $(SHARED) -o $@ $(RL_KOBJS) \
		$(SKYNET_LINK) $(SYSTEM_LIBS) $(RL_CORE_IMPLIB) $(RL_CORE_RPATH)

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

# Facade objects that are not a module entry point -- rl_common.c (the shared
# object model, linked into every module) and geo_sem.c (the geoset semantics
# layer, linked into geo.so only). They take the Lua headers, so they need a
# rule of their own rather than the kernel one; the two pattern rules coexist
# because make picks whichever prerequisite actually exists.
$(RL_OBJ_DIR)/%.o: $(RL_FACADE)/%.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(RL_CFLAGS) -I$(RL_KERNEL) -I$(RL_FACADE) -c $< -o $@

# Facade modules land in the redis/ sub namespace: luaclib/redis/{bitmap,geo,
# hll,sortedset}.so, required as "redis.bitmap" and friends (same precedent as
# NetBull's luaclib/skiplist/c.so).
$(LUA_CLIB_DIR)/redis:
	mkdir -p $@

# A module that needs more than rl_common.o lists the extra objects in
# RL_MODULE_OBJS, resolved per target by GNU make (geo.so takes geo_sem.o).
$(LUA_CLIB_DIR)/redis/geo.so: $(RL_OBJ_DIR)/geo_sem.o
$(LUA_CLIB_DIR)/redis/geo.so: RL_MODULE_OBJS := $(RL_OBJ_DIR)/geo_sem.o

$(LUA_CLIB_DIR)/redis/%.so: $(RL_FACADE)/%.c $(RL_OBJ_DIR)/rl_common.o $(RL_CORE_SO) \
	| $(LUA_CLIB_DIR) $(LUA_CLIB_DIR)/redis $(NB_SKYNET_DEP)
	$(CC) $(RL_CFLAGS) $(SHARED) -I$(RL_KERNEL) -I$(RL_FACADE) -o $@ $< \
		$(RL_OBJ_DIR)/rl_common.o $(RL_MODULE_OBJS) \
		$(RL_CORE_LINK) $(SKYNET_LINK) $(LUA_LIBS) $(SYSTEM_LIBS) $(RL_RPATH)

# LUA_CLIB entries are appended by the facade tasks themselves, one per module
# as it becomes available:
LUA_CLIB += redis/bitmap redis/hll redis/sortedset redis/geo

endif
