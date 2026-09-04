/*
 * Copyright 2018 Joey Hewitt <joey@joeyhewitt.com>
 *
 * This file is part of libqipcrtr4msmipc.
 *
 * libqipcrtr4msmipc is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libqipcrtr4msmipc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libqipcrtr4msmipc.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/msm_ipc.h>
#include <linux/qrtr.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <signal.h>

#define IPC_ROUTER_NID_LOCAL            1 /* from ipc_router.h in MSM kernel fork */

/* helpers */
static int is_socket_msm_ipc(int sockfd);
static int translate_addr_msmipc2qipcrtr(const struct sockaddr *addr, struct sockaddr_qrtr *addr_qrtr);
static int translate_addr_qipcrtr2msmipc(const struct sockaddr *addr, struct sockaddr_msm_ipc *addr_mi);
#define DBG(fmt, ...) do { fprintf(stderr, "libqipcrtr4msmipc %s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); } while(0)
#define DIE(fmt, ...) do { \
	DBG(fmt, ##__VA_ARGS__); \
	exit(2); \
} while(0)
static inline int min(int a, int b);

/* original functions */
static int (*real_socket)(int domain, int type, int protocol);
static int (*real_bind)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
static int (*real_getsockname)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
static ssize_t (*real_sendto)(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *, socklen_t);
static ssize_t (*real_recvfrom)(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
static ssize_t (*real_recvmsg)(int sockfd, struct msghdr *msg, int flags);
static ssize_t (*real_recv)(int sockfd, void *buf, size_t len, int flags);
static int (*real_poll)(struct pollfd *fds, nfds_t nfds, int timeout);
static int (*real_ppoll)(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout, const sigset_t *sigmask);
static int (*real_epoll_ctl)(int epfd, int op, int fd, struct epoll_event *event);
static int (*real_epoll_wait)(int epfd, struct epoll_event *events, int maxevents, int timeout);
static int (*real_epoll_pwait)(int epfd, struct epoll_event *events, int maxevents, int timeout, const sigset_t *sigmask);

#define LOAD_DLSYM(n) do { real_##n = dlsym(RTLD_NEXT, #n); } while(0)
__attribute__ ((__constructor__)) static void init() {
	LOAD_DLSYM(socket);
	LOAD_DLSYM(bind);
	LOAD_DLSYM(getsockname);
	LOAD_DLSYM(sendto);
	LOAD_DLSYM(recvfrom);
	LOAD_DLSYM(recvmsg);
	LOAD_DLSYM(recv);
	LOAD_DLSYM(poll);
	LOAD_DLSYM(ppoll);
	LOAD_DLSYM(epoll_ctl);
	LOAD_DLSYM(epoll_wait);
	LOAD_DLSYM(epoll_pwait);
}

/* state storage */
// TODO store on per-socket basis; for now we assume only one socket per program
static uint32_t g_service = 0, g_instance = 0;

/*
 * QRTR_TYPE_NEW_LOOKUP support: a client (e.g. qmicli/qrtr-glib) asks the
 * control node "who provides service X" and expects a QRTR_TYPE_NEW_SERVER
 * reply. There's no real qrtr nameserver on this AF_MSM_IPC-only kernel, so
 * we answer it ourselves via the same IPC_ROUTER_IOCTL_LOOKUP_SERVER ioctl
 * getsockname() already uses, and hand the caller a synthesized reply out of
 * recvfrom()/poll() since nothing real ever arrives on the wire for it.
 */
#define MAX_PENDING_LOOKUP_REPLIES 16
static int g_pending_lookup_sockfd = -1;
static struct qrtr_ctrl_pkt g_pending_lookup_replies[MAX_PENDING_LOOKUP_REPLIES];
static int g_pending_lookup_count = 0;
static int g_pending_lookup_head = 0;

/*
 * The downstream AF_MSM_IPC implementation consumes a datagram even when
 * recv(..., MSG_PEEK) is used.  GSocket peeks first to size a datagram and
 * then reads it with recvmsg(), so preserve a consumed peek in userspace.
 */
#define MAX_PEEKED_MESSAGE_SIZE 65536
static int g_peeked_sockfd = -1;
static unsigned char g_peeked_message[MAX_PEEKED_MESSAGE_SIZE];
static ssize_t g_peeked_message_size = 0;
static struct sockaddr_qrtr g_peeked_source;

/*
 * GLib's GMainContext on this glib version watches fds via epoll, not
 * poll(2), so faking readiness means tracking epoll_ctl() registrations
 * ourselves and injecting a synthetic event out of epoll_wait().
 */
#define MAX_EPOLL_REGISTRATIONS 32
static struct {
	int epfd;
	int fd;
	struct epoll_event event; /* as registered by the caller, incl. their data */
	int in_use;
} g_epoll_registrations[MAX_EPOLL_REGISTRATIONS];

/*
 * A service=0/instance=0 lookup means "enumerate every registered service"
 * (used by qrtr-glib's QrtrBus to build its initial node/service directory).
 * IPC_ROUTER_IOCTL_LOOKUP_SERVER only supports a targeted (service,instance)
 * lookup, not enumeration, so answer a wildcard lookup by probing this list
 * of well-known QMI service IDs (see qmi-enums.h) ourselves.
 */
static const uint32_t g_known_qmi_services[] = {
	1,  /* QMI_SERVICE_WDS */
	2,  /* QMI_SERVICE_DMS */
	3,  /* QMI_SERVICE_NAS */
	5,  /* QMI_SERVICE_WMS */
	11, /* QMI_SERVICE_UIM */
	26, /* QMI_SERVICE_WDA */
	47, /* QMI_SERVICE_DPM */
};

/* intercepted functions */
int socket(int domain, int type, int protocol) {
	return real_socket(domain != AF_QIPCRTR ? domain : AF_MSM_IPC, type, protocol);
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
	if (addr->sa_family == AF_QIPCRTR) {
		// we don't have enough information to bind on AF_MSM_IPC yet
		return 0;
	} else {
		return real_bind(sockfd, addr, addrlen);
	}
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
	// getsockname() on AF_MSM_IPC causes 'Unable to handle kernel NULL pointer dereference at virtual address 00000000'
	if (is_socket_msm_ipc(sockfd)) {
		// we have to lookup with an ioctl to get the node and port we're on
		struct {
			struct server_lookup_args lookup_args;
			struct msm_ipc_server_info srv_info;
		} ioctl_args = {
			.lookup_args = {
				.port_name = { .service = g_service, .instance = g_instance, },
				.num_entries_in_array = 1,
			},
		};
		int rc = ioctl(sockfd, IPC_ROUTER_IOCTL_LOOKUP_SERVER, &ioctl_args);
		if (rc < 0 || ioctl_args.lookup_args.num_entries_found != 1) {
			DBG("server info not found");
			struct sockaddr_qrtr ret = {
				.sq_family = AF_QIPCRTR,
				.sq_node = IPC_ROUTER_NID_LOCAL,
				.sq_port = 0, // XXX we don't know, but a valid sq_node should be good enough for libqrtr in this case
			};
			memcpy(addr, &ret, min(sizeof(ret), *addrlen));
			*addrlen = sizeof(ret);
			return 0;
		}
		DBG("found server at node=%d, port=%d", ioctl_args.srv_info.node_id, ioctl_args.srv_info.port_id);
		struct sockaddr_qrtr ret = {
			.sq_family = AF_QIPCRTR,
			.sq_node = ioctl_args.srv_info.node_id,
			.sq_port = ioctl_args.srv_info.port_id,
		};
		memcpy(addr, &ret, min(sizeof(ret), *addrlen));
		*addrlen = sizeof(ret);
		return 0;
	} else {
		return real_getsockname(sockfd, addr, addrlen);
	}
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
	struct sockaddr_msm_ipc sami;

	if (translate_addr_qipcrtr2msmipc(dest_addr, &sami)) {
		struct sockaddr_qrtr *dest_addr_qrtr = (struct sockaddr_qrtr *)dest_addr;
		const struct qrtr_ctrl_pkt *qrtr_ctrl_pkt = buf;

		if (dest_addr_qrtr->sq_node == IPC_ROUTER_NID_LOCAL && dest_addr_qrtr->sq_port == QRTR_PORT_CTRL &&
			qrtr_ctrl_pkt->cmd == QRTR_TYPE_NEW_SERVER) {

			// translate to bind()
			const struct sockaddr_msm_ipc bind_addr = {
				.family = AF_MSM_IPC,
				.address = {
					.addrtype = MSM_IPC_ADDR_NAME,
					.addr = {
						.port_name = {
							.service = qrtr_ctrl_pkt->server.service,
							.instance = qrtr_ctrl_pkt->server.instance,
						},
					},
				},
			};
			if (real_bind(sockfd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
				// leave bind()'s errno for caller
				return -1;
			}
			g_service = qrtr_ctrl_pkt->server.service;
			g_instance = qrtr_ctrl_pkt->server.instance;
			return len; // indicate whole packet was sent
		}

		if (dest_addr_qrtr->sq_node == IPC_ROUTER_NID_LOCAL && dest_addr_qrtr->sq_port == QRTR_PORT_CTRL &&
			qrtr_ctrl_pkt->cmd == QRTR_TYPE_NEW_LOOKUP) {

			// answer the lookup ourselves: no real qrtr nameserver exists here
			int is_wildcard = (qrtr_ctrl_pkt->server.service == 0 && qrtr_ctrl_pkt->server.instance == 0);
			uint32_t probe_services[MAX_PENDING_LOOKUP_REPLIES];
			int num_probes;

			if (is_wildcard) {
				num_probes = sizeof(g_known_qmi_services) / sizeof(g_known_qmi_services[0]);
				memcpy(probe_services, g_known_qmi_services, sizeof(g_known_qmi_services));
			} else {
				num_probes = 1;
				probe_services[0] = qrtr_ctrl_pkt->server.service;
			}

			g_pending_lookup_count = 0;
			g_pending_lookup_head = 0;

			for (int i = 0; i < num_probes && g_pending_lookup_count < MAX_PENDING_LOOKUP_REPLIES - 1; i++) {
				struct {
					struct server_lookup_args lookup_args;
					struct msm_ipc_server_info srv_info;
				} ioctl_args = {
					.lookup_args = {
						.port_name = {
							.service = probe_services[i],
							.instance = is_wildcard ? 0 : qrtr_ctrl_pkt->server.instance,
						},
						.num_entries_in_array = 1,
					},
				};
				int rc = ioctl(sockfd, IPC_ROUTER_IOCTL_LOOKUP_SERVER, &ioctl_args);
				if (rc < 0 || ioctl_args.lookup_args.num_entries_found != 1) {
					DBG("lookup: service=%u instance=%u not found",
						probe_services[i], ioctl_args.lookup_args.port_name.instance);
					continue;
				}

				DBG("lookup: service=%u instance=%u -> node=%d port=%d",
					probe_services[i], ioctl_args.lookup_args.port_name.instance,
					ioctl_args.srv_info.node_id, ioctl_args.srv_info.port_id);

				g_pending_lookup_replies[g_pending_lookup_count++] = (struct qrtr_ctrl_pkt){
					.cmd = QRTR_TYPE_NEW_SERVER,
					.server = {
						.service = probe_services[i],
						.instance = ioctl_args.lookup_args.port_name.instance,
						.node = ioctl_args.srv_info.node_id,
						.port = ioctl_args.srv_info.port_id,
					},
				};
			}

			/*
			 * qrtr-glib's QrtrBus treats a QRTR_TYPE_NEW_SERVER packet with
			 * every field zero as "end of initial dump" (see
			 * qrtr_ctrl_message_cb() in qrtr-bus.c) -- that's how the real
			 * qrtr kernel nameserver signals it's done listing servers for
			 * a wildcard lookup. Always append it so the caller's bus init
			 * actually completes instead of waiting out its timeout.
			 */
			g_pending_lookup_replies[g_pending_lookup_count++] = (struct qrtr_ctrl_pkt){
				.cmd = QRTR_TYPE_NEW_SERVER,
			};
			g_pending_lookup_sockfd = sockfd;

			return len;
		}

		return real_sendto(sockfd, buf, len, flags, (const struct sockaddr*)&sami, sizeof(sami));
	} else {
		return real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
	}
}

/* returns 1 and fills *out_sz if a pending synthesized reply was delivered, 0 otherwise */
static int try_pop_pending_reply(int sockfd, void *buf, size_t len, struct sockaddr *src_addr, socklen_t *addrlen, ssize_t *out_sz) {
	if (g_pending_lookup_sockfd != sockfd || g_pending_lookup_head >= g_pending_lookup_count)
		return 0;

	const struct qrtr_ctrl_pkt *reply = &g_pending_lookup_replies[g_pending_lookup_head++];

	if (g_pending_lookup_head >= g_pending_lookup_count) {
		g_pending_lookup_sockfd = -1;
		g_pending_lookup_count = 0;
		g_pending_lookup_head = 0;
	}

	*out_sz = min(len, sizeof(*reply));
	memcpy(buf, reply, *out_sz);

	if (src_addr && addrlen) {
		struct sockaddr_qrtr ret = {
			.sq_family = AF_QIPCRTR,
			.sq_node = IPC_ROUTER_NID_LOCAL,
			.sq_port = QRTR_PORT_CTRL,
		};
		memcpy(src_addr, &ret, min(*addrlen, sizeof(ret)));
		*addrlen = sizeof(ret);
	}

	return 1;
}

static int try_copy_peeked_message(int sockfd, void *buf, size_t len,
	struct sockaddr *src_addr, socklen_t *addrlen, int consume, ssize_t *out_sz) {
	if (g_peeked_sockfd != sockfd)
		return 0;

	*out_sz = min(len, g_peeked_message_size);
	memcpy(buf, g_peeked_message, *out_sz);

	if (src_addr && addrlen) {
		memcpy(src_addr, &g_peeked_source, min(*addrlen, sizeof(g_peeked_source)));
		*addrlen = sizeof(g_peeked_source);
	}

	if (consume) {
		g_peeked_sockfd = -1;
		g_peeked_message_size = 0;
	}

	return 1;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
	ssize_t sz;

	if (try_pop_pending_reply(sockfd, buf, len, NULL, NULL, &sz))
		return sz;
	if (try_copy_peeked_message(sockfd, buf, len, NULL, NULL,
		!(flags & MSG_PEEK), &sz))
		return sz;

	if ((flags & MSG_PEEK) && is_socket_msm_ipc(sockfd)) {
		struct sockaddr_msm_ipc sami;
		socklen_t sami_len = sizeof(sami);

		sz = real_recvfrom(sockfd, g_peeked_message, sizeof(g_peeked_message),
			flags & ~MSG_PEEK, (struct sockaddr *)&sami, &sami_len);
		if (sz < 0)
			return sz;

		g_peeked_sockfd = sockfd;
		g_peeked_message_size = sz;
		translate_addr_msmipc2qipcrtr((const struct sockaddr *)&sami, &g_peeked_source);
		sz = min(len, sz);
		memcpy(buf, g_peeked_message, sz);
		return sz;
	}

	return real_recv(sockfd, buf, len, flags);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
	ssize_t sz;

	if (msg->msg_iovlen >= 1 &&
		try_pop_pending_reply(sockfd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len,
			(struct sockaddr *)msg->msg_name, &msg->msg_namelen, &sz)) {
		msg->msg_controllen = 0;
		msg->msg_flags = 0;
		return sz;
	}
	if (msg->msg_iovlen >= 1 &&
		try_copy_peeked_message(sockfd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len,
			(struct sockaddr *)msg->msg_name, &msg->msg_namelen,
			!(flags & MSG_PEEK), &sz)) {
		msg->msg_controllen = 0;
		msg->msg_flags = 0;
		return sz;
	}

	if (is_socket_msm_ipc(sockfd) && msg->msg_name) {
		struct sockaddr_msm_ipc sami;
		void *caller_name = msg->msg_name;
		socklen_t caller_namelen = msg->msg_namelen;

		msg->msg_name = &sami;
		msg->msg_namelen = sizeof(sami);
		sz = real_recvmsg(sockfd, msg, flags);
		msg->msg_name = caller_name;
		if (sz >= 0 && translate_addr_msmipc2qipcrtr((const struct sockaddr *)&sami,
			(struct sockaddr_qrtr *)caller_name))
			msg->msg_namelen = sizeof(struct sockaddr_qrtr);
		else
			msg->msg_namelen = caller_namelen;
		return sz;
	}

	return real_recvmsg(sockfd, msg, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
	ssize_t sz;
	struct sockaddr_msm_ipc sami;
	socklen_t sami_len = sizeof(sami);

	if (try_pop_pending_reply(sockfd, buf, len, src_addr, addrlen, &sz))
		return sz;
	if (try_copy_peeked_message(sockfd, buf, len, src_addr, addrlen,
		!(flags & MSG_PEEK), &sz))
		return sz;

	sz = real_recvfrom(sockfd, buf, len, flags, (struct sockaddr*)&sami, &sami_len);

	if (translate_addr_msmipc2qipcrtr((const struct sockaddr *)&sami, (struct sockaddr_qrtr *)src_addr)) {
		*addrlen = sizeof(struct sockaddr_qrtr);
	}

	return sz;
}

static int try_poll_shortcut(struct pollfd *fds, nfds_t nfds) {
	if (g_pending_lookup_sockfd != -1) {
		for (nfds_t i = 0; i < nfds; i++) {
			if (fds[i].fd == g_pending_lookup_sockfd) {
				fds[i].revents = POLLIN;
				return 1;
			}
		}
	}

	return 0;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
	if (try_poll_shortcut(fds, nfds))
		return 1;

	return real_poll(fds, nfds, timeout);
}

int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout, const sigset_t *sigmask) {
	if (try_poll_shortcut(fds, nfds))
		return 1;

	return real_ppoll(fds, nfds, timeout, sigmask);
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
	int rc = real_epoll_ctl(epfd, op, fd, event);

	if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
		if (rc == 0 && event) {
			int slot = -1;
			for (int i = 0; i < MAX_EPOLL_REGISTRATIONS; i++) {
				if (g_epoll_registrations[i].in_use && g_epoll_registrations[i].epfd == epfd && g_epoll_registrations[i].fd == fd) {
					slot = i;
					break;
				}
			}
			if (slot == -1) {
				for (int i = 0; i < MAX_EPOLL_REGISTRATIONS; i++) {
					if (!g_epoll_registrations[i].in_use) {
						slot = i;
						break;
					}
				}
			}
			if (slot != -1) {
				g_epoll_registrations[slot].epfd = epfd;
				g_epoll_registrations[slot].fd = fd;
				g_epoll_registrations[slot].event = *event;
				g_epoll_registrations[slot].in_use = 1;
			}
		}
	} else if (op == EPOLL_CTL_DEL) {
		for (int i = 0; i < MAX_EPOLL_REGISTRATIONS; i++) {
			if (g_epoll_registrations[i].in_use && g_epoll_registrations[i].epfd == epfd && g_epoll_registrations[i].fd == fd)
				g_epoll_registrations[i].in_use = 0;
		}
	}

	return rc;
}

/* returns >=0 (event count) if handled with a synthetic event, -1 to fall through to the real call */
static int try_epoll_shortcut(int epfd, struct epoll_event *events, int maxevents) {
	if (g_pending_lookup_sockfd == -1 || maxevents < 1)
		return -1;

	for (int i = 0; i < MAX_EPOLL_REGISTRATIONS; i++) {
		if (g_epoll_registrations[i].in_use && g_epoll_registrations[i].epfd == epfd && g_epoll_registrations[i].fd == g_pending_lookup_sockfd) {
			events[0] = g_epoll_registrations[i].event;
			events[0].events = EPOLLIN;
			return 1;
		}
	}

	return -1;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {

	int rc = try_epoll_shortcut(epfd, events, maxevents);
	if (rc >= 0)
		return rc;

	return real_epoll_wait(epfd, events, maxevents, timeout);
}

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const sigset_t *sigmask) {

	int rc = try_epoll_shortcut(epfd, events, maxevents);
	if (rc >= 0)
		return rc;

	return real_epoll_pwait(epfd, events, maxevents, timeout, sigmask);
}

/* helpers */
static int translate_addr_msmipc2qipcrtr(const struct sockaddr *addr, struct sockaddr_qrtr *addr_qrtr) {
	if (addr->sa_family != AF_MSM_IPC) {
		return 0;
	}

	const struct sockaddr_msm_ipc *addr_mi = (const struct sockaddr_msm_ipc *)addr;

	if (addr_mi->address.addrtype != MSM_IPC_ADDR_ID) {
		// as far as I can see in the kernel, this won't happen in the usecases we cover
		DIE("got something other than MSM_IPC_ADDR_ID");
	}
	struct sockaddr_qrtr ret = {
		.sq_family = AF_QIPCRTR,
		.sq_node = addr_mi->address.addr.port_addr.node_id,
		.sq_port = addr_mi->address.addr.port_addr.port_id,
	};
	*addr_qrtr = ret;
	return 1;
}

static int translate_addr_qipcrtr2msmipc(const struct sockaddr *addr, struct sockaddr_msm_ipc *addr_mi) {
	if (addr->sa_family != AF_QIPCRTR) {
		return 0;
	}

	const struct sockaddr_qrtr *addr_qrtr = (const struct sockaddr_qrtr *)addr;

	struct sockaddr_msm_ipc ret = {
		.family = AF_MSM_IPC,
		.address = {
			.addrtype = MSM_IPC_ADDR_ID,
			.addr = {
				.port_addr = {
					.node_id = addr_qrtr->sq_node,
					.port_id = addr_qrtr->sq_port,
				},
			},
		},
	};

	*addr_mi = ret;
	return 1;
}

static inline int min(int a, int b) {
	return a < b ? a : b;
}

static int is_socket_msm_ipc(int sockfd) {
	int domain;
	socklen_t length = sizeof(domain);

	if (getsockopt(sockfd, SOL_SOCKET, SO_DOMAIN, &domain, &length) < 0) {
		return 0;
	}

	return length == sizeof(domain) && domain == AF_MSM_IPC;
}
