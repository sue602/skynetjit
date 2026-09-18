//
#ifndef _SHARED_HEADER_
#define _SHARED_HEADER_

#ifdef USE_MEM_HOOK
#include "skynet_malloc.h"
#define my_malloc skynet_malloc
#define my_free skynet_free
#else
#define my_malloc malloc
#define my_free free
#endif

#endif

