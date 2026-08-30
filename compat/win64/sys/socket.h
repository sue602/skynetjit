#pragma once

/* The upstream mingw compatibility header resets FD_SETSIZE to 1024 after
 * winsock2.h has already defined fd_set. Reapply this project's configured
 * value so FD_SET/FD_ISSET use the same capacity as the fd_set layout.
 */
#include_next <sys/socket.h>

#ifndef SKYNETJIT_FD_SETSIZE
#error "SKYNETJIT_FD_SETSIZE must be supplied by the integration build"
#endif

#undef FD_SETSIZE
#define FD_SETSIZE SKYNETJIT_FD_SETSIZE
