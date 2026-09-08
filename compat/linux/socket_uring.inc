/* Included at the end of socket_server.c after its private state is visible. */

enum skynetjit_uring_opcode {
	SKYNETJIT_URING_CTRL,
	SKYNETJIT_URING_RECV,
	SKYNETJIT_URING_RECVMSG,
	SKYNETJIT_URING_SEND,
	SKYNETJIT_URING_SENDMSG,
	SKYNETJIT_URING_ACCEPT,
	SKYNETJIT_URING_CONNECT,
};

struct skynetjit_uring_op {
	struct skynetjit_uring_op *next_all;
	struct skynetjit_uring_op *next_write;
	struct socket *s;
	int id;
	int fd;
	int opcode;
	bool submitted;
	bool cancelled;
	char *buffer;
	void (*release_buffer)(void *);
	void *release_arg;
	size_t size;
	size_t offset;
	struct iovec iov;
	struct msghdr msg;
	union sockaddr_all addr;
	socklen_t addrlen;
};

static struct skynetjit_uring_op *SKYNETJIT_URING_OPS = NULL;
static bool SKYNETJIT_URING_CTRL_PENDING = false;

static struct io_uring_sqe *
skynetjit_uring_get_sqe(void) {
	struct io_uring_sqe *sqe = io_uring_get_sqe(&SKYNETJIT_URING);
	if (sqe != NULL) {
		return sqe;
	}
	if (io_uring_submit(&SKYNETJIT_URING) < 0) {
		return NULL;
	}
	return io_uring_get_sqe(&SKYNETJIT_URING);
}

static void
skynetjit_uring_link(struct skynetjit_uring_op *op) {
	op->next_all = SKYNETJIT_URING_OPS;
	SKYNETJIT_URING_OPS = op;
}

static void
skynetjit_uring_unlink(struct skynetjit_uring_op *op) {
	struct skynetjit_uring_op **current = &SKYNETJIT_URING_OPS;
	while (*current != NULL) {
		if (*current == op) {
			*current = op->next_all;
			op->next_all = NULL;
			return;
		}
		current = &(*current)->next_all;
	}
}

static void
skynetjit_uring_free_op(struct skynetjit_uring_op *op) {
	if (op == NULL) {
		return;
	}
	skynetjit_uring_unlink(op);
	if (op->release_buffer != NULL) {
		op->release_buffer(op->release_arg);
	} else {
		FREE(op->buffer);
	}
	FREE(op);
}

static struct skynetjit_uring_op *
skynetjit_uring_new_op(struct socket *s, int opcode) {
	struct skynetjit_uring_op *op = MALLOC(sizeof(*op));
	if (op == NULL) {
		return NULL;
	}
	memset(op, 0, sizeof(*op));
	op->s = s;
	op->id = s ? s->id : 0;
	op->fd = s ? s->fd : -1;
	op->opcode = opcode;
	skynetjit_uring_link(op);
	return op;
}

static int
skynetjit_uring_submit_op(struct skynetjit_uring_op *op) {
	struct io_uring_sqe *sqe = skynetjit_uring_get_sqe();
	if (sqe == NULL) {
		return -1;
	}
	switch (op->opcode) {
	case SKYNETJIT_URING_CTRL:
		io_uring_prep_poll_add(sqe, op->fd, POLLIN);
		break;
	case SKYNETJIT_URING_RECV:
		io_uring_prep_recv(sqe, op->fd, op->buffer, op->size, 0);
		break;
	case SKYNETJIT_URING_RECVMSG:
		io_uring_prep_recvmsg(sqe, op->fd, &op->msg, 0);
		break;
	case SKYNETJIT_URING_SEND:
		io_uring_prep_send(sqe, op->fd, op->buffer + op->offset,
			op->size - op->offset, MSG_NOSIGNAL);
		break;
	case SKYNETJIT_URING_SENDMSG:
		io_uring_prep_sendmsg(sqe, op->fd, &op->msg, MSG_NOSIGNAL);
		break;
	case SKYNETJIT_URING_ACCEPT:
		io_uring_prep_accept(sqe, op->fd, &op->addr.s, &op->addrlen,
			SOCK_NONBLOCK | SOCK_CLOEXEC);
		break;
	case SKYNETJIT_URING_CONNECT:
		io_uring_prep_connect(sqe, op->fd, &op->addr.s, op->addrlen);
		break;
	default:
		return -1;
	}
	io_uring_sqe_set_data(sqe, op);
	op->submitted = true;
	return 0;
}

static bool
skynetjit_uring_op_valid(const struct skynetjit_uring_op *op) {
	return !op->cancelled && op->s != NULL && op->s->id == op->id &&
		ATOM_LOAD(&op->s->type) != SOCKET_TYPE_INVALID;
}

static void
skynetjit_uring_queue_ctrl(struct socket_server *ss) {
	if (SKYNETJIT_URING_CTRL_PENDING) {
		return;
	}
	struct skynetjit_uring_op *op = skynetjit_uring_new_op(NULL, SKYNETJIT_URING_CTRL);
	if (op == NULL) {
		return;
	}
	op->fd = ss->recvctrl_fd;
	if (skynetjit_uring_submit_op(op)) {
		skynetjit_uring_free_op(op);
		return;
	}
	SKYNETJIT_URING_CTRL_PENDING = true;
}

static int
skynetjit_uring_queue_tcp_read(struct socket_server *ss, struct socket *s) {
	(void)ss;
	if (s->uring_read_pending || !s->reading) {
		return 0;
	}
	int type = ATOM_LOAD(&s->type);
	if (type != SOCKET_TYPE_CONNECTED && type != SOCKET_TYPE_BIND &&
		type != SOCKET_TYPE_HALFCLOSE_WRITE) {
		return 0;
	}
	struct skynetjit_uring_op *op = skynetjit_uring_new_op(s, SKYNETJIT_URING_RECV);
	if (op == NULL) {
		return -1;
	}
	op->size = s->p.size;
	if (op->size < MIN_READ_BUFFER) {
		op->size = MIN_READ_BUFFER;
	}
	op->buffer = MALLOC(op->size);
	if (op->buffer == NULL || skynetjit_uring_submit_op(op)) {
		skynetjit_uring_free_op(op);
		return -1;
	}
	s->uring_read_pending = true;
	return 0;
}

static int
skynetjit_uring_queue_udp_read(struct socket_server *ss, struct socket *s) {
	(void)ss;
	if (s->uring_read_pending || !s->reading ||
		ATOM_LOAD(&s->type) != SOCKET_TYPE_CONNECTED) {
		return 0;
	}
	struct skynetjit_uring_op *op = skynetjit_uring_new_op(s, SKYNETJIT_URING_RECVMSG);
	if (op == NULL) {
		return -1;
	}
	op->size = MAX_UDP_PACKAGE;
	op->buffer = MALLOC(op->size);
	if (op->buffer == NULL) {
		skynetjit_uring_free_op(op);
		return -1;
	}
	op->iov.iov_base = op->buffer;
	op->iov.iov_len = op->size;
	op->msg.msg_name = &op->addr;
	op->msg.msg_namelen = sizeof(op->addr);
	op->msg.msg_iov = &op->iov;
	op->msg.msg_iovlen = 1;
	if (skynetjit_uring_submit_op(op)) {
		skynetjit_uring_free_op(op);
		return -1;
	}
	s->uring_read_pending = true;
	return 0;
}

static int
skynetjit_uring_queue_accept(struct socket_server *ss, struct socket *s) {
	(void)ss;
	if (s->uring_accept_pending || !s->reading ||
		ATOM_LOAD(&s->type) != SOCKET_TYPE_LISTEN) {
		return 0;
	}
	struct skynetjit_uring_op *op = skynetjit_uring_new_op(s, SKYNETJIT_URING_ACCEPT);
	if (op == NULL) {
		return -1;
	}
	op->addrlen = sizeof(op->addr);
	if (skynetjit_uring_submit_op(op)) {
		skynetjit_uring_free_op(op);
		return -1;
	}
	s->uring_accept_pending = true;
	return 0;
}

static int
skynetjit_uring_begin_read(struct socket_server *ss, struct socket *s) {
	if (ATOM_LOAD(&s->type) == SOCKET_TYPE_LISTEN) {
		return skynetjit_uring_queue_accept(ss, s);
	}
	if (s->protocol == PROTOCOL_TCP) {
		return skynetjit_uring_queue_tcp_read(ss, s);
	}
	return skynetjit_uring_queue_udp_read(ss, s);
}

static int
skynetjit_uring_begin_connect(struct socket_server *ss, struct socket *s,
	const struct sockaddr *addr, socklen_t addrlen) {
	(void)ss;
	if (s->uring_connect_pending || addrlen > sizeof(((struct skynetjit_uring_op *)0)->addr)) {
		return -1;
	}
	struct skynetjit_uring_op *op = skynetjit_uring_new_op(s, SKYNETJIT_URING_CONNECT);
	if (op == NULL) {
		return -1;
	}
	memcpy(&op->addr, addr, addrlen);
	op->addrlen = addrlen;
	if (skynetjit_uring_submit_op(op)) {
		skynetjit_uring_free_op(op);
		return -1;
	}
	s->uring_connect_pending = true;
	return 0;
}

static void
skynetjit_uring_write_push(struct socket *s, struct skynetjit_uring_op *op, int priority) {
	struct skynetjit_uring_op **head = priority == PRIORITY_LOW ?
		&s->uring_low_head : &s->uring_high_head;
	struct skynetjit_uring_op **tail = priority == PRIORITY_LOW ?
		&s->uring_low_tail : &s->uring_high_tail;
	op->next_write = NULL;
	if (*tail == NULL) {
		*head = *tail = op;
	} else {
		(*tail)->next_write = op;
		*tail = op;
	}
}

static struct skynetjit_uring_op *
skynetjit_uring_write_pop(struct socket *s) {
	struct skynetjit_uring_op **head = s->uring_high_head ?
		&s->uring_high_head : &s->uring_low_head;
	struct skynetjit_uring_op **tail = s->uring_high_head ?
		&s->uring_high_tail : &s->uring_low_tail;
	struct skynetjit_uring_op *op = *head;
	if (op != NULL) {
		*head = op->next_write;
		if (*head == NULL) {
			*tail = NULL;
		}
		op->next_write = NULL;
	}
	return op;
}

static int
skynetjit_uring_start_write(struct socket_server *ss, struct socket *s) {
	(void)ss;
	if (s->uring_write_active != NULL) {
		return 0;
	}
	struct skynetjit_uring_op *op = skynetjit_uring_write_pop(s);
	if (op == NULL) {
		return 0;
	}
	s->uring_write_active = op;
	if (skynetjit_uring_submit_op(op)) {
		s->uring_write_active = NULL;
		skynetjit_uring_write_push(s, op, PRIORITY_HIGH);
		return -1;
	}
	return 0;
}

static int
skynetjit_uring_has_pending_write(struct socket *s) {
	return s->uring_write_active != NULL || s->uring_high_head != NULL ||
		s->uring_low_head != NULL;
}

static int
skynetjit_uring_enqueue_send(struct socket_server *ss, struct socket *s,
	struct request_send *request, struct socket_message *result, int priority,
	const uint8_t *udp_address) {
	struct send_object so;
	send_object_init(ss, &so, request->buffer, request->sz);
	int type = ATOM_LOAD(&s->type);
	if (socket_invalid(s, request->id) || type == SOCKET_TYPE_HALFCLOSE_WRITE ||
		type == SOCKET_TYPE_PACCEPT || s->closing) {
		so.free_func((void *)request->buffer);
		return -1;
	}
	if (type == SOCKET_TYPE_PLISTEN || type == SOCKET_TYPE_LISTEN) {
		skynet_error(NULL, "socket-server error: write to listen fd %d.", request->id);
		so.free_func((void *)request->buffer);
		return -1;
	}
	int opcode = s->protocol == PROTOCOL_TCP ? SKYNETJIT_URING_SEND : SKYNETJIT_URING_SENDMSG;
	struct skynetjit_uring_op *op = skynetjit_uring_new_op(s, opcode);
	if (op == NULL) {
		so.free_func((void *)request->buffer);
		return report_error(s, result, "out of memory for io_uring send");
	}
	op->size = so.sz;
	op->buffer = (char *)so.buffer;
	op->release_buffer = so.free_func;
	op->release_arg = (void *)request->buffer;
	if (op->buffer == NULL && op->size != 0) {
		skynetjit_uring_free_op(op);
		return report_error(s, result, "invalid io_uring send buffer");
	}
	if (opcode == SKYNETJIT_URING_SENDMSG) {
		if (udp_address == NULL) {
			udp_address = s->p.udp_address;
		}
		op->addrlen = udp_socket_address(s, udp_address, &op->addr);
		if (op->addrlen == 0) {
			skynetjit_uring_free_op(op);
			return report_error(s, result, "udp address is not configured");
		}
		op->iov.iov_base = op->buffer;
		op->iov.iov_len = op->size;
		op->msg.msg_name = &op->addr;
		op->msg.msg_namelen = op->addrlen;
		op->msg.msg_iov = &op->iov;
		op->msg.msg_iovlen = 1;
	}
	s->wb_size += op->size;
	skynetjit_uring_write_push(s, op, priority);
	if (skynetjit_uring_start_write(ss, s)) {
		return report_error(s, result, "io_uring submission failed");
	}
	if (s->wb_size >= WARNING_SIZE && s->wb_size >= s->warn_size) {
		s->warn_size = s->warn_size == 0 ? WARNING_SIZE * 2 : s->warn_size * 2;
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = s->wb_size % 1024 == 0 ? s->wb_size / 1024 : s->wb_size / 1024 + 1;
		result->data = NULL;
		return SOCKET_WARNING;
	}
	return -1;
}

static int
skynetjit_uring_send(struct socket_server *ss, struct socket_sendbuffer *buf, int priority) {
	int id = buf->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id) || s->closing) {
		free_buffer(ss, buf);
		return -1;
	}
	inc_sending_ref(s, id);
	struct request_package request;
	request_init(&request);
	request.u.send.id = id;
	request.u.send.buffer = clone_buffer(buf, &request.u.send.sz);
	send_request(ss, &request, priority == PRIORITY_LOW ? 'P' : 'D',
		sizeof(request.u.send));
	return 0;
}

static void
skynetjit_uring_remove_queued_write(struct socket *s, struct skynetjit_uring_op *target) {
	struct skynetjit_uring_op **heads[2] = { &s->uring_high_head, &s->uring_low_head };
	struct skynetjit_uring_op **tails[2] = { &s->uring_high_tail, &s->uring_low_tail };
	int i;
	for (i = 0; i < 2; i++) {
		struct skynetjit_uring_op **current = heads[i];
		struct skynetjit_uring_op *previous = NULL;
		while (*current != NULL) {
			if (*current == target) {
				*current = target->next_write;
				if (tails[i] && *tails[i] == target) {
					*tails[i] = previous;
				}
				target->next_write = NULL;
				return;
			}
			previous = *current;
			current = &(*current)->next_write;
		}
	}
}

static void
skynetjit_uring_forget_socket(struct socket *s) {
	int id = s->id;
	struct skynetjit_uring_op *op = SKYNETJIT_URING_OPS;
	while (op != NULL) {
		struct skynetjit_uring_op *next = op->next_all;
		if (op->s == s && op->id == id) {
			if (op->submitted) {
				op->cancelled = true;
				op->s = NULL;
			} else {
				skynetjit_uring_remove_queued_write(s, op);
				skynetjit_uring_free_op(op);
			}
		}
		op = next;
	}
	s->uring_read_pending = false;
	s->uring_accept_pending = false;
	s->uring_connect_pending = false;
	s->uring_write_active = NULL;
	s->uring_high_head = s->uring_high_tail = NULL;
	s->uring_low_head = s->uring_low_tail = NULL;
}

static int
skynetjit_uring_finish_tcp_read(struct socket_server *ss, struct skynetjit_uring_op *op,
	int res, struct socket_message *result) {
	struct socket *s = op->s;
	s->uring_read_pending = false;
	if (res < 0) {
		skynetjit_uring_free_op(op);
		if (res == -EINTR || res == -EAGAIN || res == -ECANCELED) {
			return -1;
		}
		struct socket_lock l;
		struct socket_message ignored;
		socket_lock_init(s, &l);
		force_close(ss, s, &l, &ignored);
		return report_error(s, result, strerror(-res));
	}
	if (res == 0) {
		skynetjit_uring_free_op(op);
		if (s->closing) {
			if (nomore_sending_data(s)) {
				struct socket_lock l;
				struct socket_message ignored;
				socket_lock_init(s, &l);
				force_close(ss, s, &l, &ignored);
			}
			return -1;
		}
		if (halfclose_read(s)) {
			return -1;
		}
		if (ATOM_LOAD(&s->type) == SOCKET_TYPE_HALFCLOSE_WRITE) {
			struct socket_lock l;
			struct socket_message ignored;
			socket_lock_init(s, &l);
			force_close(ss, s, &l, &ignored);
			return SOCKET_CLOSE;
		}
		close_read(ss, s, result);
		return SOCKET_CLOSE;
	}
	if (halfclose_read(s) || !s->reading) {
		skynetjit_uring_free_op(op);
		return -1;
	}
	stat_read(ss, s, res);
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = res;
	result->data = op->buffer;
	op->buffer = NULL;
	if ((size_t)res == op->size && s->p.size < MAX_UDP_PACKAGE) {
		s->p.size *= 2;
	} else if (s->p.size > MIN_READ_BUFFER && res * 2 < s->p.size) {
		s->p.size /= 2;
	}
	skynetjit_uring_free_op(op);
	(void)skynetjit_uring_begin_read(ss, s);
	return SOCKET_DATA;
}

static int
skynetjit_uring_finish_udp_read(struct socket_server *ss, struct skynetjit_uring_op *op,
	int res, struct socket_message *result) {
	struct socket *s = op->s;
	s->uring_read_pending = false;
	if (res < 0) {
		skynetjit_uring_free_op(op);
		if (res == -EINTR || res == -EAGAIN || res == -ECANCELED) {
			return -1;
		}
		struct socket_lock l;
		struct socket_message ignored;
		socket_lock_init(s, &l);
		force_close(ss, s, &l, &ignored);
		return report_error(s, result, strerror(-res));
	}
	int family = op->addr.s.sa_family;
	int addrsz = family == AF_INET ? 1 + 2 + 4 : 1 + 2 + 16;
	if ((family == AF_INET && s->protocol != PROTOCOL_UDP) ||
		(family == AF_INET6 && s->protocol != PROTOCOL_UDPv6)) {
		skynetjit_uring_free_op(op);
		(void)skynetjit_uring_begin_read(ss, s);
		return -1;
	}
	uint8_t *data = MALLOC(res + addrsz);
	if (data == NULL) {
		skynetjit_uring_free_op(op);
		return report_error(s, result, "out of memory for io_uring udp receive");
	}
	memcpy(data, op->buffer, res);
	gen_udp_address(s->protocol, &op->addr, data + res);
	stat_read(ss, s, res);
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = res;
	result->data = (char *)data;
	skynetjit_uring_free_op(op);
	(void)skynetjit_uring_begin_read(ss, s);
	return SOCKET_UDP;
}

static int
skynetjit_uring_finish_accept(struct socket_server *ss, struct skynetjit_uring_op *op,
	int res, struct socket_message *result) {
	struct socket *s = op->s;
	s->uring_accept_pending = false;
	if (res < 0) {
		skynetjit_uring_free_op(op);
		if (res == -EINTR || res == -EAGAIN || res == -ECANCELED) {
			return -1;
		}
		return report_error(s, result, strerror(-res));
	}
	int client_fd = res;
	int id = reserve_id(ss);
	if (id < 0) {
		close(client_fd);
		skynetjit_uring_free_op(op);
		(void)skynetjit_uring_queue_accept(ss, s);
		return -1;
	}
	socket_keepalive(client_fd);
	struct socket *ns = new_fd(ss, id, client_fd, PROTOCOL_TCP, s->opaque, false);
	if (ns == NULL) {
		close(client_fd);
		ATOM_STORE(&ss->slot[HASH_ID(id)].type, SOCKET_TYPE_INVALID);
		skynetjit_uring_free_op(op);
		(void)skynetjit_uring_queue_accept(ss, s);
		return -1;
	}
	ATOM_STORE(&ns->type, SOCKET_TYPE_PACCEPT);
	stat_read(ss, s, 1);
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = id;
	result->data = NULL;
	if (getname(&op->addr, ss->buffer, sizeof(ss->buffer))) {
		result->data = ss->buffer;
	}
	skynetjit_uring_free_op(op);
	(void)skynetjit_uring_queue_accept(ss, s);
	return SOCKET_ACCEPT;
}

static int
skynetjit_uring_finish_connect(struct socket_server *ss, struct skynetjit_uring_op *op,
	int res, struct socket_message *result) {
	struct socket *s = op->s;
	s->uring_connect_pending = false;
	skynetjit_uring_free_op(op);
	if (res < 0) {
		struct socket_lock l;
		struct socket_message ignored;
		socket_lock_init(s, &l);
		force_close(ss, s, &l, &ignored);
		return report_error(s, result, strerror(-res));
	}
	struct socket_lock l;
	socket_lock_init(s, &l);
	int type = report_connect(ss, s, &l, result);
	if (type == SOCKET_OPEN) {
		(void)skynetjit_uring_begin_read(ss, s);
	}
	return type;
}

static int
skynetjit_uring_finish_write(struct socket_server *ss, struct skynetjit_uring_op *op,
	int res, struct socket_message *result) {
	struct socket *s = op->s;
	assert(s->uring_write_active == op);
	if (res <= 0 && op->size != 0) {
		s->uring_write_active = NULL;
		skynetjit_uring_free_op(op);
		struct socket_lock l;
		struct socket_message ignored;
		socket_lock_init(s, &l);
		force_close(ss, s, &l, &ignored);
		return report_error(s, result, res < 0 ? strerror(-res) : "io_uring send returned zero");
	}
	if (res > 0) {
		op->offset += res;
		s->wb_size -= res;
		stat_write(ss, s, res);
	}
	if (op->offset < op->size) {
		if (skynetjit_uring_submit_op(op)) {
			s->uring_write_active = NULL;
			skynetjit_uring_free_op(op);
			return report_error(s, result, "io_uring send resubmission failed");
		}
		return -1;
	}
	s->uring_write_active = NULL;
	skynetjit_uring_free_op(op);
	if (skynetjit_uring_start_write(ss, s)) {
		return report_error(s, result, "io_uring send submission failed");
	}
	if (!skynetjit_uring_has_pending_write(s)) {
		if (s->closing) {
			struct socket_lock l;
			struct socket_message ignored;
			socket_lock_init(s, &l);
			force_close(ss, s, &l, &ignored);
			return -1;
		}
		if (s->warn_size > 0) {
			s->warn_size = 0;
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = NULL;
			return SOCKET_WARNING;
		}
	}
	return -1;
}

static int
skynetjit_uring_complete(struct socket_server *ss, struct skynetjit_uring_op *op,
	int res, struct socket_message *result) {
	if (op == NULL) {
		return -1;
	}
	op->submitted = false;
	if (op->opcode == SKYNETJIT_URING_CTRL) {
		SKYNETJIT_URING_CTRL_PENDING = false;
		skynetjit_uring_free_op(op);
		return -1;
	}
	if (!skynetjit_uring_op_valid(op)) {
		skynetjit_uring_free_op(op);
		return -1;
	}
	switch (op->opcode) {
	case SKYNETJIT_URING_RECV:
		return skynetjit_uring_finish_tcp_read(ss, op, res, result);
	case SKYNETJIT_URING_RECVMSG:
		return skynetjit_uring_finish_udp_read(ss, op, res, result);
	case SKYNETJIT_URING_ACCEPT:
		return skynetjit_uring_finish_accept(ss, op, res, result);
	case SKYNETJIT_URING_CONNECT:
		return skynetjit_uring_finish_connect(ss, op, res, result);
	case SKYNETJIT_URING_SEND:
	case SKYNETJIT_URING_SENDMSG:
		return skynetjit_uring_finish_write(ss, op, res, result);
	default:
		skynetjit_uring_free_op(op);
		return -1;
	}
}

static int
skynetjit_uring_poll(struct socket_server *ss, struct socket_message *result, int *more) {
	for (;;) {
		skynetjit_uring_queue_ctrl(ss);
		if (has_cmd(ss)) {
			int type = ctrl_cmd(ss, result);
			if (type != -1) {
				return type;
			}
			continue;
		}
		int submitted = io_uring_submit_and_wait(&SKYNETJIT_URING, 1);
		if (submitted < 0) {
			skynet_error(NULL, "socket-server io_uring wait failed: %s", strerror(-submitted));
			continue;
		}
		struct io_uring_cqe *cqe;
		while (io_uring_peek_cqe(&SKYNETJIT_URING, &cqe) == 0) {
			struct skynetjit_uring_op *op = io_uring_cqe_get_data(cqe);
			int res = cqe->res;
			io_uring_cqe_seen(&SKYNETJIT_URING, cqe);
			int type = skynetjit_uring_complete(ss, op, res, result);
			if (type != -1) {
				if (more != NULL) {
					*more = 0;
				}
				return type;
			}
		}
	}
}

static void
skynetjit_uring_dispose(void) {
	struct skynetjit_uring_op *op;
	for (op = SKYNETJIT_URING_OPS; op != NULL; op = op->next_all) {
		if (op->submitted) {
			struct io_uring_sqe *sqe = skynetjit_uring_get_sqe();
			if (sqe != NULL) {
				io_uring_prep_cancel(sqe, op, 0);
				io_uring_sqe_set_data(sqe, NULL);
			}
		}
	}
	(void)io_uring_submit(&SKYNETJIT_URING);
	while (SKYNETJIT_URING_OPS != NULL) {
		struct io_uring_cqe *cqe;
		if (io_uring_wait_cqe(&SKYNETJIT_URING, &cqe) < 0) {
			break;
		}
		struct skynetjit_uring_op *completed = io_uring_cqe_get_data(cqe);
		io_uring_cqe_seen(&SKYNETJIT_URING, cqe);
		if (completed != NULL) {
			skynetjit_uring_free_op(completed);
		}
	}
	while (SKYNETJIT_URING_OPS != NULL) {
		skynetjit_uring_free_op(SKYNETJIT_URING_OPS);
	}
	SKYNETJIT_URING_CTRL_PENDING = false;
}
