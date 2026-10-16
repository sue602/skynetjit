/* skynetjit_alloc.h -- 本仓库决定"谁持有堆"的唯一位置。
 *
 * 上游的 skynet-src/skynet_malloc.h 把 skynet_malloc 等七个入口直接宏到 CRT
 * malloc，于是 malloc_hook.c 里那一整套 cookie 与按服务记账的代码整条编译不
 * 出去（那个分支本来只服务于 jemalloc）。实测结果：skynet.memory 的
 * total/current/block、debug_console 的 mem、launcher 的 MEM 全部读 0，哪怕
 * 此刻有一条几 MB 的消息缓冲区活着。
 *
 * 定义 SKYNETJIT_ALLOC_WRAPPED 之后宏让位给真函数，记账随之活过来，后端由此
 * 处的七个宏决定；换分配器只需要改这个文件。
 *
 * skynet_lalloc（Lua VM 的分配器）一直不是宏，它经 malloc_hook.c 的
 * raw_realloc/raw_free 走同一个后端，所以不在此列。
 */

#ifndef SKYNETJIT_ALLOC_H
#define SKYNETJIT_ALLOC_H

#include <stdio.h>
#include <stdlib.h>

/* 后端：libc。 */
#define SKYNETJIT_MALLOC(sz)          malloc(sz)
#define SKYNETJIT_REALLOC(p, sz)      realloc((p), (sz))
#define SKYNETJIT_CALLOC(n, sz)       calloc((n), (sz))
#define SKYNETJIT_FREE(p)             free(p)

#if defined(_WIN32)
/* 对齐分配的这三个入口在本 fork 里没有任何调用点：skynet-src、service-src、
 * lualib-src、redislib-src、luaclib-src 都 grep 过，只剩声明与定义。Windows 的
 * CRT 只提供 _aligned_malloc，而它必须配 _aligned_free，接不进唯一的
 * skynet_free；硬接进去等于埋一个"哪天有人调用就未定义"的坑，所以这里显式中止，
 * 而不是给一个看着能用的假实现。 */
static inline void
skynetjit_no_aligned(const char *what) {
	fprintf(stderr, "skynetjit: %s is not implemented for the Windows CRT backend: "
		"its blocks cannot be released by skynet_free\n", what);
	fflush(stderr);
	abort();
}
#define SKYNETJIT_MEMALIGN(alignment, size) \
	((void)(alignment), (void)(size), skynetjit_no_aligned("skynet_memalign"), NULL)
#define SKYNETJIT_ALIGNED_ALLOC(alignment, size) \
	((void)(alignment), (void)(size), skynetjit_no_aligned("skynet_aligned_alloc"), NULL)
#define SKYNETJIT_POSIX_MEMALIGN(memptr, alignment, size) \
	((void)(memptr), (void)(alignment), (void)(size), \
		skynetjit_no_aligned("skynet_posix_memalign"), -1)
#else
/* posix_memalign 出来的块本来就能用 free() 释放，三个入口因此共用一个后端：
 * aligned_alloc 在 C11 还额外要求 size 是 alignment 的整数倍，而
 * malloc_hook.c 会往 size 上加 cookie，用 posix_memalign 反而更少一个坑。
 * alignment 须为 2 的幂且是 sizeof(void*) 的整数倍，与 jemalloc 的 memalign
 * 同样的约束。 */
static inline void *
skynetjit_aligned(size_t alignment, size_t size) {
	void *p = NULL;
	if (posix_memalign(&p, alignment, size) != 0) {
		return NULL;
	}
	return p;
}
#define SKYNETJIT_MEMALIGN(alignment, size) skynetjit_aligned((alignment), (size))
#define SKYNETJIT_ALIGNED_ALLOC(alignment, size) skynetjit_aligned((alignment), (size))
#define SKYNETJIT_POSIX_MEMALIGN(memptr, alignment, size) \
	posix_memalign((memptr), (alignment), (size))
#endif

#endif /* SKYNETJIT_ALLOC_H */
