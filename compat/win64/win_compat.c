#define SKYNETJIT_COMPAT_IMPLEMENTATION
#include "compat.h"
#include "wepoll.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>

enum handle_kind {
	HANDLE_KIND_NONE = 0,
	HANDLE_KIND_SOCKET = 1,
	HANDLE_KIND_EPOLL = 2,
};

struct handle_entry {
	uintptr_t value;
	unsigned char kind;
};

/* Keep the initial control-pipe tokens below Skynet's FD_SETSIZE assertion. */
#define HANDLE_ID_BASE 256

static INIT_ONCE registry_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK registry_lock = SRWLOCK_INIT;
static struct handle_entry *registry_entries;
static size_t registry_capacity;

/* Winsock polling cannot wait on a console HANDLE. A reader thread forwards
 * fd 0 into this socket pair so the unmodified Skynet socket loop can poll it.
 */
static INIT_ONCE stdin_bridge_once = INIT_ONCE_STATIC_INIT;
static SOCKET stdin_bridge_reader = INVALID_SOCKET;
static SOCKET stdin_bridge_writer = INVALID_SOCKET;
static HANDLE stdin_bridge_thread_handle;
static volatile LONG stdin_bridge_started;

static BOOL CALLBACK
registry_init(PINIT_ONCE once, PVOID parameter, PVOID *context) {
	WSADATA data;
	(void)once;
	(void)parameter;
	(void)context;
	return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

static int
ensure_registry(void) {
	return InitOnceExecuteOnce(&registry_once, registry_init, NULL, NULL) ? 0 : -1;
}

static void
set_errno_from_wsa(int error) {
	switch (error) {
	case WSAEWOULDBLOCK: errno = EAGAIN; break;
	case WSAEINTR: errno = EINTR; break;
	case WSAEINVAL: errno = EINVAL; break;
	case WSAEACCES: errno = EACCES; break;
	case WSAECONNRESET: errno = ECONNRESET; break;
	case WSAECONNABORTED: errno = ECONNABORTED; break;
	case WSAECONNREFUSED: errno = ECONNREFUSED; break;
	case WSAENETDOWN: errno = ENETDOWN; break;
	case WSAENETUNREACH: errno = ENETUNREACH; break;
	case WSAEHOSTUNREACH: errno = EHOSTUNREACH; break;
	case WSAETIMEDOUT: errno = ETIMEDOUT; break;
	case WSAENOTCONN: errno = ENOTCONN; break;
	case WSAEADDRINUSE: errno = EADDRINUSE; break;
	case WSAEADDRNOTAVAIL: errno = EADDRNOTAVAIL; break;
	case WSAENOTSOCK: errno = EBADF; break;
	default: errno = EIO; break;
	}
}

static int
registry_add(uintptr_t value, enum handle_kind kind) {
	size_t index;
	int id = -1;
	if (ensure_registry() != 0)
		return -1;
	AcquireSRWLockExclusive(&registry_lock);
	for (index = 0; index < registry_capacity; ++index) {
		if (registry_entries[index].kind == HANDLE_KIND_NONE)
			break;
	}
	if (index == registry_capacity) {
		size_t old_capacity = registry_capacity;
		size_t new_capacity = old_capacity ? old_capacity * 2 : 256;
		struct handle_entry *new_entries;
		if (new_capacity > (size_t)(INT_MAX - HANDLE_ID_BASE))
			goto done;
		new_entries = (struct handle_entry *)realloc(
			registry_entries, new_capacity * sizeof(*new_entries));
		if (new_entries == NULL)
			goto done;
		memset(new_entries + old_capacity, 0,
		       (new_capacity - old_capacity) * sizeof(*new_entries));
		registry_entries = new_entries;
		registry_capacity = new_capacity;
		index = old_capacity;
	}
	registry_entries[index].value = value;
	registry_entries[index].kind = (unsigned char)kind;
	id = HANDLE_ID_BASE + (int)index;
done:
	ReleaseSRWLockExclusive(&registry_lock);
	if (id < 0)
		errno = EMFILE;
	return id;
}

static int
registry_get(int id, enum handle_kind kind, uintptr_t *value) {
	size_t index;
	int found = 0;
	if (id < HANDLE_ID_BASE)
		return 0;
	index = (size_t)(id - HANDLE_ID_BASE);
	AcquireSRWLockShared(&registry_lock);
	if (index < registry_capacity && registry_entries[index].kind == kind) {
		*value = registry_entries[index].value;
		found = 1;
	}
	ReleaseSRWLockShared(&registry_lock);
	return found;
}

static int
registry_take(int id, enum handle_kind *kind, uintptr_t *value) {
	size_t index;
	int found = 0;
	if (id < HANDLE_ID_BASE)
		return 0;
	index = (size_t)(id - HANDLE_ID_BASE);
	AcquireSRWLockExclusive(&registry_lock);
	if (index < registry_capacity &&
	    registry_entries[index].kind != HANDLE_KIND_NONE) {
		*kind = (enum handle_kind)registry_entries[index].kind;
		*value = registry_entries[index].value;
		registry_entries[index].kind = HANDLE_KIND_NONE;
		registry_entries[index].value = 0;
		found = 1;
	}
	ReleaseSRWLockExclusive(&registry_lock);
	return found;
}

static int
native_socket_pair(SOCKET pair[2]) {
	struct sockaddr_in address;
	int address_size = (int)sizeof(address);
	SOCKET listener = INVALID_SOCKET;
	SOCKET writer = INVALID_SOCKET;
	SOCKET reader = INVALID_SOCKET;

	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;

	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET ||
	    bind(listener, (struct sockaddr *)&address, sizeof(address)) ==
		SOCKET_ERROR ||
	    listen(listener, 1) == SOCKET_ERROR ||
	    getsockname(listener, (struct sockaddr *)&address, &address_size) ==
		SOCKET_ERROR)
		goto failed;

	writer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (writer == INVALID_SOCKET ||
	    connect(writer, (struct sockaddr *)&address, address_size) ==
		SOCKET_ERROR)
		goto failed;
	reader = accept(listener, NULL, NULL);
	if (reader == INVALID_SOCKET)
		goto failed;

	closesocket(listener);
	pair[0] = reader;
	pair[1] = writer;
	return 0;

failed:
	{
		int error = WSAGetLastError();
		if (listener != INVALID_SOCKET) closesocket(listener);
		if (writer != INVALID_SOCKET) closesocket(writer);
		if (reader != INVALID_SOCKET) closesocket(reader);
		set_errno_from_wsa(error);
	}
	return -1;
}

static unsigned __stdcall
stdin_bridge_worker(void *parameter) {
	SOCKET writer = (SOCKET)(uintptr_t)parameter;
	char buffer[4096];

	for (;;) {
		int count = _read(0, buffer, sizeof(buffer));
		int offset = 0;
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		while (offset < count) {
			int sent = send(writer, buffer + offset, count - offset, 0);
			if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEINTR)
				continue;
			if (sent <= 0)
				goto finished;
			offset += sent;
		}
	}

finished:
	shutdown(writer, SD_SEND);
	return 0;
}

static BOOL CALLBACK
stdin_bridge_init(PINIT_ONCE once, PVOID parameter, PVOID *context) {
	SOCKET pair[2];
	uintptr_t thread;
	(void)once;
	(void)parameter;
	(void)context;

	if (ensure_registry() != 0 || native_socket_pair(pair) != 0)
		return FALSE;
	thread = _beginthreadex(NULL, 0, stdin_bridge_worker,
				(void *)(uintptr_t)pair[1], 0, NULL);
	if (thread == 0) {
		closesocket(pair[0]);
		closesocket(pair[1]);
		errno = EAGAIN;
		return FALSE;
	}

	stdin_bridge_reader = pair[0];
	stdin_bridge_writer = pair[1];
	stdin_bridge_thread_handle = (HANDLE)thread;
	InterlockedExchange(&stdin_bridge_started, 1);
	return TRUE;
}

static SOCKET
stdin_socket_value(void) {
	if (!InitOnceExecuteOnce(&stdin_bridge_once, stdin_bridge_init,
				 NULL, NULL)) {
		if (errno == 0) errno = EIO;
		return INVALID_SOCKET;
	}
	return stdin_bridge_reader;
}

static int
stdin_bridge_close(void) {
	DWORD wait_result = WAIT_OBJECT_0;
	if (!InterlockedExchange(&stdin_bridge_started, 0))
		return _close(0);

	if (stdin_bridge_writer != INVALID_SOCKET)
		shutdown(stdin_bridge_writer, SD_BOTH);
	if (stdin_bridge_thread_handle != NULL) {
		CancelSynchronousIo(stdin_bridge_thread_handle);
		wait_result = WaitForSingleObject(stdin_bridge_thread_handle, 1000);
		CloseHandle(stdin_bridge_thread_handle);
		stdin_bridge_thread_handle = NULL;
	}
	if (stdin_bridge_writer != INVALID_SOCKET) {
		closesocket(stdin_bridge_writer);
		stdin_bridge_writer = INVALID_SOCKET;
	}
	if (stdin_bridge_reader != INVALID_SOCKET) {
		closesocket(stdin_bridge_reader);
		stdin_bridge_reader = INVALID_SOCKET;
	}
	if (wait_result == WAIT_FAILED) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static SOCKET
socket_value(int fd) {
	uintptr_t value;
	if (fd == 0)
		return stdin_socket_value();
	if (registry_get(fd, HANDLE_KIND_SOCKET, &value))
		return (SOCKET)value;
	/* Compatibility for callers that still pass a low-valued native socket. */
	return (SOCKET)(uintptr_t)(unsigned int)fd;
}

static HANDLE
epoll_value(int token) {
	uintptr_t value;
	if (registry_get(token, HANDLE_KIND_EPOLL, &value))
		return (HANDLE)value;
	return (HANDLE)(intptr_t)token;
}

int
skynetjit_socket(int af, int type, int protocol) {
	SOCKET value;
	int id;
	if (ensure_registry() != 0)
		return -1;
	value = socket(af, type, protocol);
	if (value == INVALID_SOCKET) {
		set_errno_from_wsa(WSAGetLastError());
		return -1;
	}
	id = registry_add((uintptr_t)value, HANDLE_KIND_SOCKET);
	if (id < 0)
		closesocket(value);
	return id;
}

int
skynetjit_accept(int fd, struct sockaddr *addr, int *addrlen) {
	SOCKET value = accept(socket_value(fd), addr, addrlen);
	int id;
	if (value == INVALID_SOCKET) {
		set_errno_from_wsa(WSAGetLastError());
		return -1;
	}
	id = registry_add((uintptr_t)value, HANDLE_KIND_SOCKET);
	if (id < 0)
		closesocket(value);
	return id;
}

#define SOCKET_CALL(name, signature, callargs) \
	int skynetjit_##name signature { \
		int result = name callargs; \
		if (result == SOCKET_ERROR) set_errno_from_wsa(WSAGetLastError()); \
		return result; \
	}

SOCKET_CALL(bind, (int fd, const struct sockaddr *name, int namelen),
	    (socket_value(fd), name, namelen))
SOCKET_CALL(listen, (int fd, int backlog), (socket_value(fd), backlog))
SOCKET_CALL(connect, (int fd, const struct sockaddr *name, int namelen),
	    (socket_value(fd), name, namelen))
SOCKET_CALL(shutdown, (int fd, int how), (socket_value(fd), how))
SOCKET_CALL(getpeername, (int fd, struct sockaddr *name, int *namelen),
	    (socket_value(fd), name, namelen))
SOCKET_CALL(getsockname, (int fd, struct sockaddr *name, int *namelen),
	    (socket_value(fd), name, namelen))

int
skynetjit_send(int fd, const void *buf, int len, int flags) {
	if (len == 0)
		return 0;
	int result = send(socket_value(fd), (const char *)buf, len, flags);
	if (result == SOCKET_ERROR)
		set_errno_from_wsa(WSAGetLastError());
	return result;
}

int
skynetjit_sendto(int fd, const void *buf, int len, int flags,
	      const struct sockaddr *to, int tolen) {
	int result = sendto(socket_value(fd), (const char *)buf, len, flags,
			    to, tolen);
	if (result == SOCKET_ERROR)
		set_errno_from_wsa(WSAGetLastError());
	return result;
}

int
skynetjit_recv(int fd, void *buf, int len, int flags) {
	if (len == 0)
		return 0;
	int result = recv(socket_value(fd), (char *)buf, len, flags);
	if (result == SOCKET_ERROR)
		set_errno_from_wsa(WSAGetLastError());
	return result;
}

int
skynetjit_recvfrom(int fd, void *buf, int len, int flags,
		struct sockaddr *from, int *fromlen) {
	int result = recvfrom(socket_value(fd), (char *)buf, len, flags,
			      from, fromlen);
	if (result == SOCKET_ERROR)
		set_errno_from_wsa(WSAGetLastError());
	return result;
}

int
skynetjit_setsockopt(int fd, int level, int optname, const void *optval,
		  int optlen) {
	int result = setsockopt(socket_value(fd), level, optname,
				(const char *)optval, optlen);
	if (result == SOCKET_ERROR)
		set_errno_from_wsa(WSAGetLastError());
	return result;
}

int
skynetjit_getsockopt(int fd, int level, int optname, void *optval,
		  int *optlen) {
	int result = getsockopt(socket_value(fd), level, optname,
				(char *)optval, optlen);
	if (result == SOCKET_ERROR)
		set_errno_from_wsa(WSAGetLastError());
	return result;
}

int
skynetjit_ioctlsocket(int fd, long cmd, u_long *argp) {
	int result = ioctlsocket(socket_value(fd), cmd, argp);
	if (result == SOCKET_ERROR)
		set_errno_from_wsa(WSAGetLastError());
	return result;
}

struct select_map {
	SOCKET token;
	SOCKET value;
};

static int
translate_fd_set(fd_set *source, fd_set *target, struct select_map *map) {
	u_int i;
	int count = 0;
	FD_ZERO(target);
	if (source == NULL)
		return 0;
	for (i = 0; i < source->fd_count; ++i) {
		int token = (int)(intptr_t)source->fd_array[i];
		SOCKET value = socket_value(token);
		map[count].token = source->fd_array[i];
		map[count].value = value;
		FD_SET(value, target);
		++count;
	}
	return count;
}

static void
restore_fd_set(fd_set *destination, const fd_set *ready,
	       const struct select_map *map, int count) {
	int i;
	if (destination == NULL)
		return;
	FD_ZERO(destination);
	for (i = 0; i < count; ++i) {
		if (FD_ISSET(map[i].value, ready))
			FD_SET(map[i].token, destination);
	}
}

int
skynetjit_select(int nfds, fd_set *readfds, fd_set *writefds,
	      fd_set *exceptfds, const struct timeval *timeout) {
	fd_set real_read, real_write, real_except;
	struct select_map read_map[FD_SETSIZE];
	struct select_map write_map[FD_SETSIZE];
	struct select_map except_map[FD_SETSIZE];
	int read_count = translate_fd_set(readfds, &real_read, read_map);
	int write_count = translate_fd_set(writefds, &real_write, write_map);
	int except_count = translate_fd_set(exceptfds, &real_except, except_map);
	int result;
	(void)nfds;
	result = select(0, readfds ? &real_read : NULL,
			writefds ? &real_write : NULL,
			exceptfds ? &real_except : NULL, timeout);
	if (result == SOCKET_ERROR) {
		set_errno_from_wsa(WSAGetLastError());
		return -1;
	}
	restore_fd_set(readfds, &real_read, read_map, read_count);
	restore_fd_set(writefds, &real_write, write_map, write_count);
	restore_fd_set(exceptfds, &real_except, except_map, except_count);
	return result;
}

int
skynetjit_epoll_create(int size) {
	HANDLE value = epoll_create(size);
	int id;
	if (value == NULL)
		return -1;
	id = registry_add((uintptr_t)value, HANDLE_KIND_EPOLL);
	if (id < 0) {
		epoll_close(value);
		return -1;
	}
	return id;
}

int
skynetjit_epoll_create1(int flags) {
	return skynetjit_epoll_create(flags);
}

int
skynetjit_epoll_close(int epfd) {
	return skynetjit_close(epfd);
}

int
skynetjit_epoll_ctl(int epfd, int op, int sock,
		 struct epoll_event *event) {
	return epoll_ctl(epoll_value(epfd), op, socket_value(sock), event);
}

int
skynetjit_epoll_wait(int epfd, struct epoll_event *events,
		  int maxevents, int timeout) {
	return epoll_wait(epoll_value(epfd), events, maxevents, timeout);
}

int
skynetjit_close(int fd) {
	enum handle_kind kind;
	uintptr_t value;
	if (fd == 0)
		return stdin_bridge_close();
	if (registry_take(fd, &kind, &value)) {
		if (kind == HANDLE_KIND_SOCKET)
			return closesocket((SOCKET)value);
		if (kind == HANDLE_KIND_EPOLL)
			return epoll_close((HANDLE)value);
	}
	if (closesocket((SOCKET)(uintptr_t)(unsigned int)fd) == 0)
		return 0;
	if (WSAGetLastError() != WSAENOTSOCK)
		set_errno_from_wsa(WSAGetLastError());
	return _close(fd);
}

int
skynetjit_read(int fd, void *buffer, unsigned int size) {
	uintptr_t value;
	if (size == 0)
		return 0;
	if (fd == 0)
		return skynetjit_recv(fd, buffer, (int)size, 0);
	if (registry_get(fd, HANDLE_KIND_SOCKET, &value))
		return skynetjit_recv(fd, buffer, (int)size, 0);
	return _read(fd, buffer, size);
}

int
skynetjit_write(int fd, const void *buffer, unsigned int size) {
	uintptr_t value;
	if (size == 0)
		return 0;
	if (registry_get(fd, HANDLE_KIND_SOCKET, &value))
		return skynetjit_send(fd, buffer, (int)size, 0);
	return _write(fd, buffer, size);
}

int
skynetjit_pipe(int fd[2]) {
	struct sockaddr_in address;
	int address_len = (int)sizeof(address);
	int listener = -1;
	int writer = -1;
	int reader = -1;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	listener = skynetjit_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener < 0 || skynetjit_bind(listener, (struct sockaddr *)&address,
					(int)sizeof(address)) != 0 ||
	    skynetjit_listen(listener, 1) != 0 ||
	    skynetjit_getsockname(listener, (struct sockaddr *)&address,
			       &address_len) != 0)
		goto failed;
	writer = skynetjit_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (writer < 0 || skynetjit_connect(writer, (struct sockaddr *)&address,
					 address_len) != 0)
		goto failed;
	reader = skynetjit_accept(listener, NULL, NULL);
	if (reader < 0)
		goto failed;
	skynetjit_close(listener);
	fd[0] = reader;
	fd[1] = writer;
	return 0;
failed:
	if (listener >= 0) skynetjit_close(listener);
	if (writer >= 0) skynetjit_close(writer);
	if (reader >= 0) skynetjit_close(reader);
	return -1;
}

int
skynetjit_fcntl(int fd, int cmd, long arg) {
	if (cmd == F_GETFL)
		return 0;
	if (cmd == F_SETFL && (arg & O_NONBLOCK)) {
		u_long enabled = 1;
		return skynetjit_ioctlsocket(fd, FIONBIO, &enabled);
	}
	return 0;
}

void
skynetjit_usleep(size_t usec) {
	DWORD msec = (DWORD)((usec + 999) / 1000);
	Sleep(msec);
}

void
skynetjit_sleep(size_t sec) {
	Sleep((DWORD)(sec * 1000));
}

int
skynetjit_kill(int pid, int exit_code) {
	DWORD access = exit_code == 0 ? PROCESS_QUERY_LIMITED_INFORMATION
				    : PROCESS_TERMINATE;
	HANDLE process = OpenProcess(access, FALSE, (DWORD)pid);
	int result;
	if (process == NULL) {
		errno = ESRCH;
		return -1;
	}
	result = exit_code == 0 ? 0
				: (TerminateProcess(process, (UINT)exit_code) ? 0 : -1);
	CloseHandle(process);
	return result;
}

int skynetjit_daemon(int nochdir, int noclose) {
	(void)nochdir;
	(void)noclose;
	return 0;
}

int skynetjit_flock(int fd, int operation) {
	(void)fd;
	(void)operation;
	return 0;
}

char *
skynetjit_strsep(char **stringp, const char *delim) {
	char *start = *stringp;
	char *cursor;
	if (start == NULL)
		return NULL;
	cursor = start;
	while (*cursor != '\0') {
		const char *d;
		for (d = delim; *d != '\0'; ++d) {
			if (*cursor == *d) {
				*cursor = '\0';
				*stringp = cursor + 1;
				return start;
			}
		}
		++cursor;
	}
	*stringp = NULL;
	return start;
}

struct tm *
skynetjit_localtime_r(const time_t *timer, struct tm *result) {
	return _localtime64_s(result, timer) == 0 ? result : NULL;
}

int skynetjit_sigfillset(int *set) {
	if (set) *set = -1;
	return 0;
}

int skynetjit_sigemptyset(int *set) {
	if (set) *set = 0;
	return 0;
}

int
skynetjit_sigaction(int signum, const struct skynetjit_sigaction *act,
		 struct skynetjit_sigaction *oldact) {
	(void)signum;
	(void)act;
	(void)oldact;
	return 0;
}
