#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <errno.h>
#include <io.h>
#include <process.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#endif
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif

#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SA_RESTART
#define SA_RESTART 0x0002
#endif
#ifndef LOCK_EX
#define LOCK_EX 2
#endif
#ifndef LOCK_NB
#define LOCK_NB 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 1
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef F_GETFL
#define F_GETFL 3
#endif

struct skynetjit_sigaction {
	void (*sa_handler)(int);
	int sa_flags;
	int sa_mask;
};

struct epoll_event;

int skynetjit_socket(int af, int type, int protocol);
int skynetjit_accept(int fd, struct sockaddr *addr, int *addrlen);
int skynetjit_bind(int fd, const struct sockaddr *name, int namelen);
int skynetjit_listen(int fd, int backlog);
int skynetjit_connect(int fd, const struct sockaddr *name, int namelen);
int skynetjit_send(int fd, const void *buf, int len, int flags);
int skynetjit_sendto(int fd, const void *buf, int len, int flags,
		  const struct sockaddr *to, int tolen);
int skynetjit_recv(int fd, void *buf, int len, int flags);
int skynetjit_recvfrom(int fd, void *buf, int len, int flags,
		    struct sockaddr *from, int *fromlen);
int skynetjit_shutdown(int fd, int how);
int skynetjit_setsockopt(int fd, int level, int optname, const void *optval,
		      int optlen);
int skynetjit_getsockopt(int fd, int level, int optname, void *optval,
		      int *optlen);
int skynetjit_getpeername(int fd, struct sockaddr *name, int *namelen);
int skynetjit_getsockname(int fd, struct sockaddr *name, int *namelen);
int skynetjit_ioctlsocket(int fd, long cmd, u_long *argp);
int skynetjit_select(int nfds, fd_set *readfds, fd_set *writefds,
		  fd_set *exceptfds, const struct timeval *timeout);

int skynetjit_epoll_create(int size);
int skynetjit_epoll_create1(int flags);
int skynetjit_epoll_close(int epfd);
int skynetjit_epoll_ctl(int epfd, int op, int sock,
		     struct epoll_event *event);
int skynetjit_epoll_wait(int epfd, struct epoll_event *events,
		      int maxevents, int timeout);

int skynetjit_close(int fd);
int skynetjit_read(int fd, void *buffer, unsigned int size);
int skynetjit_write(int fd, const void *buffer, unsigned int size);
int skynetjit_pipe(int fd[2]);
int skynetjit_fcntl(int fd, int cmd, long arg);
void skynetjit_usleep(size_t usec);
void skynetjit_sleep(size_t sec);
int skynetjit_kill(int pid, int exit_code);
int skynetjit_daemon(int nochdir, int noclose);
int skynetjit_flock(int fd, int operation);
char *skynetjit_strsep(char **stringp, const char *delim);
struct tm *skynetjit_localtime_r(const time_t *timer, struct tm *result);
int skynetjit_sigfillset(int *set);
int skynetjit_sigemptyset(int *set);
int skynetjit_sigaction(int signum, const struct skynetjit_sigaction *act,
		     struct skynetjit_sigaction *oldact);

#ifndef SKYNETJIT_COMPAT_IMPLEMENTATION
#define socket skynetjit_socket
#define accept skynetjit_accept
#define bind skynetjit_bind
#define listen skynetjit_listen
#define connect skynetjit_connect
#define send skynetjit_send
#define sendto skynetjit_sendto
#define recv skynetjit_recv
#define recvfrom skynetjit_recvfrom
#define shutdown skynetjit_shutdown
#define closesocket skynetjit_close
#define setsockopt skynetjit_setsockopt
#define getsockopt skynetjit_getsockopt
#define getpeername skynetjit_getpeername
#define getsockname skynetjit_getsockname
#define ioctlsocket skynetjit_ioctlsocket
#define select skynetjit_select

#define epoll_create skynetjit_epoll_create
#define epoll_create1 skynetjit_epoll_create1
#define epoll_close skynetjit_epoll_close
#define epoll_ctl skynetjit_epoll_ctl
#define epoll_wait skynetjit_epoll_wait

#define close skynetjit_close
#define read skynetjit_read
#define write skynetjit_write
#define pipe skynetjit_pipe
#define fcntl skynetjit_fcntl
#define usleep skynetjit_usleep
#define sleep skynetjit_sleep
#define kill skynetjit_kill
#define daemon skynetjit_daemon
#define flock skynetjit_flock
#define strsep skynetjit_strsep
#define localtime_r skynetjit_localtime_r
#define sigfillset skynetjit_sigfillset
#define sigemptyset skynetjit_sigemptyset
#define sigaction skynetjit_sigaction
#define random rand
#define srandom srand
#endif
