# Shared source lists; platform-specific compilation and linkage stay in each Makefile.
CSERVICE := snlua logger gate harbor
LUA_CLIB := skynet client bson md5 sproto lpeg

SKYNET_SRC := skynet_handle.c skynet_module.c skynet_mq.c \
	skynet_server.c skynet_start.c skynet_timer.c skynet_error.c \
	skynet_harbor.c skynet_env.c skynet_monitor.c skynet_socket.c \
	socket_server.c mem_info.c malloc_hook.c skynet_daemon.c skynet_log.c

LUA_CLIB_SKYNET := lua-skynet.c lua-seri.c lua-socket.c lua-mongo.c \
	lua-netpack.c lua-memory.c lua-multicast.c lua-cluster.c lua-crypt.c \
	lsha1.c lua-sharedata.c lua-stm.c lua-debugchannel.c lua-datasheet.c
