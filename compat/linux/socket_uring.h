/*
 * Completion-driven io_uring support for the Skynet socket server.
 *
 * This header replaces the readiness helpers from socket_poll.h only when
 * SKYNETJIT_USE_IO_URING is defined. The operation implementation lives in
 * socket_uring.inc, which is included after socket_server.c has defined its
 * private socket state.
 */
#ifndef SKYNETJIT_SOCKET_URING_H
#define SKYNETJIT_SOCKET_URING_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SKYNETJIT_URING_QUEUE_DEPTH
#define SKYNETJIT_URING_QUEUE_DEPTH 32768
#endif

struct socket;
struct socket_server;
struct socket_message;
struct request_send;
struct socket_sendbuffer;
struct skynetjit_uring_op;

static struct io_uring SKYNETJIT_URING;
static bool SKYNETJIT_URING_READY = false;

static void skynetjit_uring_dispose(void);
static int skynetjit_uring_poll(struct socket_server *, struct socket_message *, int *);
static int skynetjit_uring_begin_read(struct socket_server *, struct socket *);
static int skynetjit_uring_begin_connect(struct socket_server *, struct socket *,
	const struct sockaddr *, socklen_t);
static int skynetjit_uring_enqueue_send(struct socket_server *, struct socket *,
	struct request_send *, struct socket_message *, int, const uint8_t *);
static int skynetjit_uring_send(struct socket_server *, struct socket_sendbuffer *, int);
static void skynetjit_uring_forget_socket(struct socket *);
static int skynetjit_uring_has_pending_write(struct socket *);

static bool
sp_invalid(poll_fd fd) {
	return fd < 0;
}

static poll_fd
sp_create(void) {
	if (io_uring_queue_init(SKYNETJIT_URING_QUEUE_DEPTH, &SKYNETJIT_URING, 0) < 0) {
		return -1;
	}
	SKYNETJIT_URING_READY = true;
	fprintf(stderr, "[skynetjit] io_uring completion backend enabled (depth=%d)\n",
		SKYNETJIT_URING_QUEUE_DEPTH);
	return 0;
}

static void
sp_release(poll_fd fd) {
	(void)fd;
	if (!SKYNETJIT_URING_READY) {
		return;
	}
	skynetjit_uring_dispose();
	io_uring_queue_exit(&SKYNETJIT_URING);
	SKYNETJIT_URING_READY = false;
}

/* socket_server_poll_uring owns operation submission. */
static int
sp_add(poll_fd fd, int sock, void *ud) {
	(void)fd;
	(void)sock;
	(void)ud;
	return SKYNETJIT_URING_READY ? 0 : 1;
}

static void
sp_del(poll_fd fd, int sock) {
	(void)fd;
	(void)sock;
}

static int
sp_enable(poll_fd fd, int sock, void *ud, bool read_enable, bool write_enable) {
	(void)fd;
	(void)sock;
	(void)ud;
	(void)read_enable;
	(void)write_enable;
	return SKYNETJIT_URING_READY ? 0 : 1;
}

static int
sp_wait(poll_fd fd, struct event *e, int max) {
	(void)fd;
	(void)e;
	(void)max;
	errno = ENOSYS;
	return -1;
}

static void
sp_nonblocking(int fd) {
	int flag = fcntl(fd, F_GETFL, 0);
	if (flag >= 0) {
		(void)fcntl(fd, F_SETFL, flag | O_NONBLOCK);
	}
}

#endif
