/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libubox/uloop.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#if defined(MBEDTLS_SSL_PROTO_TLS1_3) || defined(MBEDTLS_USE_PSA_CRYPTO)
#include <psa/crypto.h>
#endif

#define MESH_DISCOVERY_PORT 41007
#define MESH_HEARTBEAT_PORT 41008
#define MESH_ANNOUNCE_MS 5000
#define MESH_HEARTBEAT_MS 3000
#define MESH_CONNECTION_MS 2500
#define MESH_CONNECTION_MAX 16
#define MESH_ACCEPT_BUDGET 8
#define MESH_IO_BUDGET 32
#define MESH_UDP_BUDGET 16
#define MESH_NETWORK_MAX 128
#define MESH_MODEL_MAX 63
#define MESH_PKI_FILE_MAX (64 * 1024)
#define MESH_SHA256_LEN 32
#define MESH_NETWORK_TAG_LEN 16
#define MESH_SEEN_MAX 32
#define MESH_DEDUPE_MS 2000
#define MESH_FRAME_HEADER 2

/* Discovery is public. The network tag groups hints, not credentials. Wire:
 * magic[4], role[1], SHA256(network)[0:16], SHA256(leaf DER)[32], model_len[1],
 * model[model_len]. No address is carried in (or accepted from) the payload.
 */
#define DISCOVERY_ROLE_OFFSET 4
#define DISCOVERY_TAG_OFFSET (DISCOVERY_ROLE_OFFSET + 1)
#define DISCOVERY_ID_OFFSET (DISCOVERY_TAG_OFFSET + MESH_NETWORK_TAG_LEN)
#define DISCOVERY_SIZE_OFFSET (DISCOVERY_ID_OFFSET + MESH_SHA256_LEN)
#define DISCOVERY_HEADER (DISCOVERY_SIZE_OFFSET + 1)
#define DISCOVERY_CANDIDATE 1
#define DISCOVERY_GATEWAY 2

struct mesh_udp {
	struct uloop_fd io;
	struct mesh_transport *owner;
	struct in_addr destination;
};

struct mesh_seen {
	unsigned char id[MESH_SHA256_LEN];
	struct in_addr ip;
	uint64_t at_ms;
};

enum mesh_phase {
	CONNECTING,
	HANDSHAKING,
	READ_HEADER,
	READ_PAYLOAD,
	WRITE_FRAME,
	CLOSING,
};

struct mesh_connection {
	struct mesh_transport *owner;
	struct uloop_fd io;
	struct uloop_timeout deadline;
	mbedtls_ssl_context ssl;
	enum mesh_phase phase;
	unsigned int io_budget;
	char peer_id[65];
	char peer_ip[INET_ADDRSTRLEN];
	unsigned char header[MESH_FRAME_HEADER];
	unsigned char input[MESH_FRAME_MAX];
	unsigned char output[MESH_FRAME_HEADER + MESH_FRAME_MAX];
	size_t offset;
	size_t length;
};

struct mesh_transport {
	struct mesh_transport_ops ops;
	void *arg;
	bool gateway;
	bool candidate;
	bool stopping;
	unsigned int dispatch_depth;
	unsigned int ifindex;
	struct sockaddr_in management;
	struct sockaddr_in broadcast;
	struct sockaddr_in gateway_address;
	unsigned char local_id[MESH_SHA256_LEN];
	unsigned char gateway_id[MESH_SHA256_LEN];
	unsigned char announcement[DISCOVERY_HEADER + MESH_MODEL_MAX];
	size_t announcement_len;
	struct mesh_seen seen[MESH_SEEN_MAX];
	unsigned int next_seen;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context rng;
	mbedtls_x509_crt ca;
	mbedtls_x509_crt local_cert;
	mbedtls_pk_context key;
	mbedtls_ssl_config tls;
	struct mesh_udp udp_local;
	struct mesh_udp udp_broadcast;
	struct uloop_fd listener;
	struct uloop_timeout announce;
	struct uloop_timeout heartbeat;
	unsigned int connection_limit;
	struct mesh_connection connections[];
};

static const unsigned char discovery_magic[] = { 'M', 'S', 'H', 1 };

/* TLS 1.2, when available, also uses certificate-authenticated ephemeral
 * key exchange and AEAD only. TLS 1.3 is restricted separately to (EC)DHE.
 */
static const int tls_suites[] = {
	MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
	MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	0
};

static bool nonempty(const char *s)
{
	return s && *s;
}

static bool valid_unicast(struct in_addr ip)
{
	/* 0/8, multicast and reserved/broadcast ranges are not management hosts. */
	uint32_t host = ntohl(ip.s_addr);

	return host >= UINT32_C(0x01000000) && host < UINT32_C(0xe0000000);
}

static bool parse_unicast(const char *s, struct in_addr *ip)
{
	return nonempty(s) && inet_pton(AF_INET, s, ip) == 1 && valid_unicast(*ip);
}

static unsigned int management_ifindex(struct in_addr ip, const struct in_addr *broadcast)
{
	struct ifaddrs *all, *ifa;
	unsigned int index = 0;
	bool broadcast_ok = !broadcast || broadcast->s_addr == htonl(INADDR_BROADCAST);

	if (getifaddrs(&all) < 0)
		return 0;
	for (ifa = all; ifa; ifa = ifa->ifa_next) {
		unsigned int found;

		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET ||
		    ((const struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr != ip.s_addr)
			continue;
		found = if_nametoindex(ifa->ifa_name);
		if (!found || (index && found != index)) {
			index = 0;
			break;
		}
		index = found;
		if (broadcast && (ifa->ifa_flags & IFF_BROADCAST) && ifa->ifa_broadaddr &&
		    ifa->ifa_broadaddr->sa_family == AF_INET &&
		    ((const struct sockaddr_in *)ifa->ifa_broadaddr)->sin_addr.s_addr ==
		    broadcast->s_addr)
			broadcast_ok = true;
	}
	freeifaddrs(all);
	return broadcast_ok ? index : 0;
}

static void id_to_hex(const unsigned char id[MESH_SHA256_LEN], char out[65])
{
	static const char hex[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < MESH_SHA256_LEN; i++) {
		out[2 * i] = hex[id[i] >> 4];
		out[2 * i + 1] = hex[id[i] & 15];
	}
	out[64] = '\0';
}

static int hex_value(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool hex_to_id(const char *s, unsigned char id[MESH_SHA256_LEN])
{
	size_t i;

	if (!s || strnlen(s, 65) != 64)
		return false;
	for (i = 0; i < MESH_SHA256_LEN; i++) {
		int high = hex_value((unsigned char)s[i * 2]);
		int low = hex_value((unsigned char)s[i * 2 + 1]);

		if (high < 0 || low < 0)
			return false;
		id[i] = (unsigned char)((high << 4) | low);
	}
	return true;
}

static bool model_valid(const unsigned char *s, size_t len)
{
	size_t i;

	if (!len || len > MESH_MODEL_MAX)
		return false;
	for (i = 0; i < len; i++)
		if (s[i] < 0x20 || s[i] > 0x7e)
			return false;
	return true;
}

static bool cert_id(const mbedtls_x509_crt *cert, unsigned char out[MESH_SHA256_LEN])
{
	return cert && cert->raw.p && cert->raw.len &&
	       mbedtls_sha256(cert->raw.p, cert->raw.len, out, 0) == 0;
}

static bool leaf_valid_for(const mbedtls_x509_crt *cert, bool server)
{
	if (!cert || !cert->raw.p || mbedtls_x509_crt_get_ca_istrue(cert) == 1 ||
	    mbedtls_x509_crt_check_key_usage(cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE))
		return false;
	return (server ?
		mbedtls_x509_crt_check_extended_key_usage(cert, MBEDTLS_OID_SERVER_AUTH,
							 MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)) :
		mbedtls_x509_crt_check_extended_key_usage(cert, MBEDTLS_OID_CLIENT_AUTH,
							 MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH))) == 0;
}

/* Do not clear any X.509 verification flags. The exact leaf pin is an extra
 * check, not a trust anchor. Do it also during certificate verification so
 * the node need not present its client certificate to an unpinned server.
 */
static int verify_gateway_pin(void *arg, mbedtls_x509_crt *cert, int depth, uint32_t *flags)
{
	const struct mesh_transport *t = arg;
	unsigned char id[MESH_SHA256_LEN];

	if (depth == 0 && (!leaf_valid_for(cert, true) || !cert_id(cert, id) ||
			   memcmp(id, t->gateway_id, sizeof(id)) != 0))
		*flags |= MBEDTLS_X509_BADCERT_OTHER;
	return 0;
}

static void cancel_timeout(struct uloop_timeout *tm)
{
	if (tm->pending)
		uloop_timeout_cancel(tm);
}

static void close_io(struct uloop_fd *io)
{
	if (io->registered)
		uloop_fd_delete(io);
	if (io->fd >= 0) {
		close(io->fd);
		io->fd = -1;
	}
}

static void close_connection(struct mesh_connection *c)
{
	if (c->io.fd < 0)
		return;
	cancel_timeout(&c->deadline);
	close_io(&c->io);
	mbedtls_ssl_free(&c->ssl);
	mbedtls_platform_zeroize(c->input, sizeof(c->input));
	mbedtls_platform_zeroize(c->output, sizeof(c->output));
	c->peer_id[0] = '\0';
	c->peer_ip[0] = '\0';
}

static void destroy_transport(struct mesh_transport *t)
{
	unsigned int i;

	cancel_timeout(&t->heartbeat);
	cancel_timeout(&t->announce);
	close_io(&t->listener);
	close_io(&t->udp_broadcast.io);
	close_io(&t->udp_local.io);
	for (i = 0; i < t->connection_limit; i++)
		close_connection(&t->connections[i]);
	mbedtls_ssl_config_free(&t->tls);
	mbedtls_pk_free(&t->key);
	mbedtls_x509_crt_free(&t->local_cert);
	mbedtls_x509_crt_free(&t->ca);
	mbedtls_ctr_drbg_free(&t->rng);
	mbedtls_entropy_free(&t->entropy);
	free(t);
}

static void finish_dispatch(struct mesh_transport *t)
{
	if (--t->dispatch_depth == 0 && t->stopping)
		destroy_transport(t);
}

static int bind_interface(int fd, unsigned int ifindex)
{
#ifdef SO_BINDTOIFINDEX
	int index = (int)ifindex;

	return setsockopt(fd, SOL_SOCKET, SO_BINDTOIFINDEX, &index, sizeof(index));
#else
	char name[IF_NAMESIZE];

	if (!if_indextoname(ifindex, name))
		return -1;
	return setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name, strlen(name) + 1);
#endif
}

static int udp_socket(struct mesh_udp *u, struct in_addr address, bool send, bool receive)
{
	struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_port = receive ? htons(MESH_DISCOVERY_PORT) : 0,
		.sin_addr = address,
	};
	int one = 1;

	u->destination = address;
	u->io.fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (u->io.fd < 0)
		return -1;
	if (bind_interface(u->io.fd, u->owner->ifindex) ||
	    (send && setsockopt(u->io.fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one))) ||
	    (receive && setsockopt(u->io.fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one))) ||
	    bind(u->io.fd, (const struct sockaddr *)&local, sizeof(local)))
		return -1;
	return 0;
}

static void announce_cb(struct uloop_timeout *tm)
{
	struct mesh_transport *t = container_of(tm, struct mesh_transport, announce);

	/* Address and interface are explicitly bound; only the destination is
	 * broadcast. A lost beacon (e.g. full send queue) is harmless.
	 */
	(void)sendto(t->udp_local.io.fd, t->announcement, t->announcement_len, MSG_DONTWAIT,
		     (const struct sockaddr *)&t->broadcast, sizeof(t->broadcast));
	uloop_timeout_set(tm, MESH_ANNOUNCE_MS);
}

static bool seen_recently(struct mesh_transport *t, const unsigned char *id, struct in_addr ip)
{
	struct timespec now;
	uint64_t ms;
	struct mesh_seen *slot = NULL;
	unsigned int i;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return false;
	ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
	for (i = 0; i < MESH_SEEN_MAX; i++) {
		struct mesh_seen *s = &t->seen[i];

		if (s->ip.s_addr == ip.s_addr && memcmp(s->id, id, sizeof(s->id)) == 0) {
			if (ms - s->at_ms < MESH_DEDUPE_MS)
				return true;
			slot = s;
			break;
		}
	}
	if (!slot)
		slot = &t->seen[t->next_seen++ % MESH_SEEN_MAX];
	memcpy(slot->id, id, sizeof(slot->id));
	slot->ip = ip;
	slot->at_ms = ms;
	return false;
}

static bool received_on_management(struct msghdr *msg, const struct mesh_udp *u)
{
	struct cmsghdr *cmsg;

	for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
		struct in_pktinfo info;

		if (cmsg->cmsg_level != IPPROTO_IP || cmsg->cmsg_type != IP_PKTINFO ||
		    cmsg->cmsg_len != CMSG_LEN(sizeof(info)))
			continue;
		memcpy(&info, CMSG_DATA(cmsg), sizeof(info));
		return (unsigned int)info.ipi_ifindex == u->owner->ifindex &&
		       info.ipi_addr.s_addr == u->destination.s_addr;
	}
	return false;
}

static void discovery_cb(struct uloop_fd *fd, unsigned int events)
{
	struct mesh_udp *u = container_of(fd, struct mesh_udp, io);
	struct mesh_transport *t = u->owner;
	unsigned int i;

	if (!(events & ULOOP_READ) || !t->ops.discovered)
		return;
	t->dispatch_depth++;
	for (i = 0; i < MESH_UDP_BUDGET && !t->stopping; i++) {
		unsigned char packet[DISCOVERY_HEADER + MESH_MODEL_MAX];
		union {
			struct cmsghdr align;
			unsigned char bytes[CMSG_SPACE(sizeof(struct in_pktinfo))];
		} control;
		struct sockaddr_in source = { 0 };
		struct iovec iov = { .iov_base = packet, .iov_len = sizeof(packet) };
		struct msghdr msg = {
			.msg_name = &source,
			.msg_namelen = sizeof(source),
			.msg_iov = &iov,
			.msg_iovlen = 1,
			.msg_control = control.bytes,
			.msg_controllen = sizeof(control.bytes),
		};
		char id[65], ip[INET_ADDRSTRLEN], model[MESH_MODEL_MAX + 1];
		const unsigned char *raw_id = packet + DISCOVERY_ID_OFFSET;
		size_t model_len;
		ssize_t n = recvmsg(fd->fd, &msg, MSG_DONTWAIT);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || n < DISCOVERY_HEADER ||
		    msg.msg_namelen != sizeof(source) || source.sin_family != AF_INET ||
		    !valid_unicast(source.sin_addr) ||
		    source.sin_addr.s_addr == t->management.sin_addr.s_addr ||
		    !received_on_management(&msg, u) ||
		    memcmp(packet, discovery_magic, sizeof(discovery_magic)) ||
		    packet[DISCOVERY_ROLE_OFFSET] != DISCOVERY_CANDIDATE ||
		    memcmp(packet + DISCOVERY_TAG_OFFSET, t->announcement + DISCOVERY_TAG_OFFSET,
			   MESH_NETWORK_TAG_LEN) ||
		    memcmp(raw_id, t->local_id, MESH_SHA256_LEN) == 0)
			continue;
		model_len = packet[DISCOVERY_SIZE_OFFSET];
		if ((size_t)n != DISCOVERY_HEADER + model_len ||
		    !model_valid(packet + DISCOVERY_HEADER, model_len) ||
		    !inet_ntop(AF_INET, &source.sin_addr, ip, sizeof(ip)) ||
		    seen_recently(t, raw_id, source.sin_addr))
			continue;
		id_to_hex(raw_id, id);
		memcpy(model, packet + DISCOVERY_HEADER, model_len);
		model[model_len] = '\0';
		t->ops.discovered(t->arg, id, ip, model);
	}
	finish_dispatch(t);
}

static int tls_send(void *ctx, const unsigned char *data, size_t len)
{
	struct mesh_connection *c = ctx;
	ssize_t n;

	if (!c->io_budget)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	c->io_budget--;
	n = send(c->io.fd, data, len > INT_MAX ? INT_MAX : len, MSG_NOSIGNAL);
	if (n > 0)
		return (int)n;
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_recv(void *ctx, unsigned char *data, size_t len)
{
	struct mesh_connection *c = ctx;
	ssize_t n;

	if (!c->io_budget)
		return MBEDTLS_ERR_SSL_WANT_READ;
	c->io_budget--;
	n = recv(c->io.fd, data, len > INT_MAX ? INT_MAX : len, 0);
	if (n >= 0)
		return (int)n;
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
		return MBEDTLS_ERR_SSL_WANT_READ;
	return MBEDTLS_ERR_NET_RECV_FAILED;
}

static bool watch_connection(struct mesh_connection *c, unsigned int events)
{
	unsigned int flags = events | ULOOP_ERROR_CB;

	return !c->io.error && !(events == ULOOP_READ && c->io.eof) &&
	       ((c->io.registered && c->io.flags == flags) || uloop_fd_add(&c->io, flags) == 0);
}

static void connection_timeout(struct uloop_timeout *tm)
{
	struct mesh_connection *c = container_of(tm, struct mesh_connection, deadline);

	close_connection(c);
}

static bool setup_connection(struct mesh_connection *c, int fd, enum mesh_phase phase)
{
	c->io.fd = fd;
	c->io.eof = false;
	c->io.error = false;
	c->phase = phase;
	mbedtls_ssl_init(&c->ssl);
	if (mbedtls_ssl_setup(&c->ssl, &c->owner->tls))
		return false;
	/* There is no DNS name in this protocol: authenticate the configured
	 * gateway's exact certificate pin in addition to the verified CA chain.
	 * The explicit NULL opts out of DNS matching only, not chain validation.
	 */
	if (!c->owner->gateway && mbedtls_ssl_set_hostname(&c->ssl, NULL))
		return false;
	mbedtls_ssl_set_bio(&c->ssl, c, tls_send, tls_recv, NULL);
	return uloop_timeout_set(&c->deadline, MESH_CONNECTION_MS) == 0;
}

static bool authorize_peer(struct mesh_connection *c)
{
	struct mesh_transport *t = c->owner;
	const mbedtls_x509_crt *peer;
	unsigned char id[MESH_SHA256_LEN];
	unsigned int i;

	if (mbedtls_ssl_get_verify_result(&c->ssl) != 0)
		return false;
	peer = mbedtls_ssl_get_peer_cert(&c->ssl);
	if (!leaf_valid_for(peer, !t->gateway) || !cert_id(peer, id) ||
	    memcmp(id, t->local_id, sizeof(id)) == 0)
		return false;
	if (!t->gateway)
		return memcmp(id, t->gateway_id, sizeof(id)) == 0;

	id_to_hex(id, c->peer_id);
	/* A single active exchange per enrolled identity. */
	for (i = 0; i < t->connection_limit; i++) {
		struct mesh_connection *other = &t->connections[i];

		if (other != c && other->io.fd >= 0 && strcmp(other->peer_id, c->peer_id) == 0)
			return false;
	}
	return t->ops.is_allowed(t->arg, c->peer_id) && !t->stopping;
}

static void begin_read(struct mesh_connection *c)
{
	c->phase = READ_HEADER;
	c->offset = 0;
	c->length = sizeof(c->header);
}

static bool begin_write(struct mesh_connection *c, size_t payload_len)
{
	if (c->owner->stopping || !payload_len || payload_len > MESH_FRAME_MAX)
		return false;
	c->output[0] = (unsigned char)(payload_len >> 8);
	c->output[1] = (unsigned char)payload_len;
	c->phase = WRITE_FRAME;
	c->offset = 0;
	c->length = MESH_FRAME_HEADER + payload_len;
	return true;
}

/* One frame per direction, 2-byte big-endian length and 1..2048 raw bytes.
 * Do not consume a second frame or call any application handler more than once.
 * The connection's absolute monotonic deadline includes connect, handshake,
 * all partial reads/writes and TLS close, and is never renewed by traffic.
 */
static void drive_connection(struct mesh_connection *c)
{
	struct mesh_transport *t = c->owner;
	int rc;

	c->io_budget = MESH_IO_BUDGET;
	for (;;) {
		switch (c->phase) {
		case CONNECTING: {
			int error = 0;
			socklen_t len = sizeof(error);

			if (getsockopt(c->io.fd, SOL_SOCKET, SO_ERROR, &error, &len) || error)
				goto close;
			c->phase = HANDSHAKING;
			continue;
		}
		case HANDSHAKING:
			rc = mbedtls_ssl_handshake(&c->ssl);
			if (rc != 0)
				break;
			if (!authorize_peer(c))
				goto close;
			if (t->gateway) {
				begin_read(c);
			} else if (!begin_write(c, t->ops.node_request(t->arg,
							c->output + MESH_FRAME_HEADER, MESH_FRAME_MAX))) {
				goto close;
			}
			continue;
		case READ_HEADER:
		case READ_PAYLOAD: {
			unsigned char *buffer = c->phase == READ_HEADER ? c->header : c->input;

			rc = mbedtls_ssl_read(&c->ssl, buffer + c->offset, c->length - c->offset);
			if (rc <= 0)
				break;
			c->offset += (size_t)rc;
			if (c->offset < c->length)
				continue;
			if (c->phase == READ_HEADER) {
				c->length = ((size_t)c->header[0] << 8) | c->header[1];
				if (!c->length || c->length > MESH_FRAME_MAX)
					goto close;
				c->phase = READ_PAYLOAD;
				c->offset = 0;
				continue;
			}
			if (mbedtls_ssl_get_bytes_avail(&c->ssl) != 0)
				goto close;
			if (t->gateway) {
				size_t response_len;

				/* Recheck before dispatch in case enrollment changed in flight. */
				if (!t->ops.is_allowed(t->arg, c->peer_id) || t->stopping)
					goto close;
				response_len = t->ops.gateway_request(t->arg, c->peer_id, c->peer_ip,
					c->input, c->length, c->output + MESH_FRAME_HEADER, MESH_FRAME_MAX);
				if (!response_len || response_len > MESH_FRAME_MAX) {
					c->phase = CLOSING;
				} else if (!begin_write(c, response_len)) {
					goto close;
				}
			} else {
				const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&c->ssl);

				if (!peer || !peer->raw.p || !peer->raw.len)
					goto close;
				t->ops.node_response(t->arg, c->input, c->length, peer->raw.p, peer->raw.len);
				c->phase = CLOSING;
			}
			if (t->stopping)
				goto close;
			continue;
		}
		case WRITE_FRAME:
			/* A WANT_READ/WRITE retry must use exactly the same buffer
			 * and length; move offset only after a positive TLS write.
			 */
			rc = mbedtls_ssl_write(&c->ssl, c->output + c->offset, c->length - c->offset);
			if (rc <= 0)
				break;
			c->offset += (size_t)rc;
			if (c->offset < c->length)
				continue;
			if (t->gateway)
				c->phase = CLOSING;
			else
				begin_read(c);
			continue;
		case CLOSING:
			rc = mbedtls_ssl_close_notify(&c->ssl);
			break;
		default:
			goto close;
		}
		if (rc == MBEDTLS_ERR_SSL_WANT_READ && watch_connection(c, ULOOP_READ))
			return;
		if (rc == MBEDTLS_ERR_SSL_WANT_WRITE && watch_connection(c, ULOOP_WRITE))
			return;
		goto close;
	}
close:
	close_connection(c);
}

static void connection_cb(struct uloop_fd *fd, unsigned int events)
{
	struct mesh_connection *c = container_of(fd, struct mesh_connection, io);
	struct mesh_transport *t = c->owner;

	(void)events;
	t->dispatch_depth++;
	drive_connection(c);
	finish_dispatch(t);
}

static void accept_cb(struct uloop_fd *fd, unsigned int events)
{
	struct mesh_transport *t = container_of(fd, struct mesh_transport, listener);
	unsigned int budget;

	if (!(events & ULOOP_READ))
		return;
	t->dispatch_depth++;
	for (budget = 0; budget < MESH_ACCEPT_BUDGET && !t->stopping; budget++) {
		struct mesh_connection *c = NULL;
		struct sockaddr_in source;
		socklen_t len = sizeof(source);
		unsigned int i;
		int client = accept4(fd->fd, (struct sockaddr *)&source, &len,
				     SOCK_NONBLOCK | SOCK_CLOEXEC);

		if (client < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			break;
		}
		for (i = 0; i < t->connection_limit; i++) {
			if (t->connections[i].io.fd < 0) {
				c = &t->connections[i];
				break;
			}
		}
		if (!c || len != sizeof(source) || source.sin_family != AF_INET ||
		    !valid_unicast(source.sin_addr) ||
		    !inet_ntop(AF_INET, &source.sin_addr, c->peer_ip, sizeof(c->peer_ip))) {
			close(client);
			continue;
		}
		if (setup_connection(c, client, HANDSHAKING))
			drive_connection(c);
		else
			close_connection(c);
	}
	finish_dispatch(t);
}

static void heartbeat_cb(struct uloop_timeout *tm)
{
	struct mesh_transport *t = container_of(tm, struct mesh_transport, heartbeat);
	struct mesh_connection *c = &t->connections[0];
	struct sockaddr_in local = t->management;
	int fd, rc;

	uloop_timeout_set(tm, MESH_HEARTBEAT_MS);
	if (c->io.fd >= 0)
		return;
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	if (bind_interface(fd, t->ifindex) || bind(fd, (const struct sockaddr *)&local, sizeof(local))) {
		close(fd);
		return;
	}
	rc = connect(fd, (const struct sockaddr *)&t->gateway_address, sizeof(t->gateway_address));
	if (rc && errno != EINPROGRESS) {
		close(fd);
		return;
	}
	t->dispatch_depth++;
	if (!setup_connection(c, fd, rc ? CONNECTING : HANDSHAKING)) {
		close_connection(c);
	} else if (!rc) {
		drive_connection(c);
	} else if (!watch_connection(c, ULOOP_WRITE)) {
		close_connection(c);
	}
	finish_dispatch(t);
}

/* Parse the same descriptor we checked. O_NONBLOCK prevents special files
 * from blocking open; O_NOFOLLOW rejects a symlink at the configured path.
 * Keep the PKI directory (and its ancestors) under administrator control.
 */
static bool parse_pki_file(struct mesh_transport *t, const char *path, mbedtls_x509_crt *cert)
{
	struct stat st;
	unsigned char *buf = NULL;
	size_t len = 0, off = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	int rc = -1;

	if (fd < 0)
		return false;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
	    !(st.st_mode & S_IRUSR) || (st.st_mode & (cert ? 0022 : 0077)) ||
	    st.st_size <= 0 || st.st_size > MESH_PKI_FILE_MAX)
		goto out;
	len = (size_t)st.st_size;
	buf = malloc(len + 1);
	if (!buf)
		goto out;
	while (off < len) {
		ssize_t n = read(fd, buf + off, len - off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			goto out;
		off += (size_t)n;
	}
	/* Both mbedTLS parsers take the terminating NUL for PEM, but not DER.
	 * Match the PEM-only text marker after optional leading whitespace.
	 */
	buf[len] = '\0';
	off = 0;
	while (off < len && (buf[off] == ' ' || buf[off] == '\t' ||
			     buf[off] == '\r' || buf[off] == '\n'))
		off++;
	if (len - off >= 11 && !memcmp(buf + off, "-----BEGIN ", 11)) {
		if (memchr(buf, '\0', len))
			goto out;
		off = len + 1;
	} else {
		off = len;
	}
	if (cert)
		rc = mbedtls_x509_crt_parse(cert, buf, off);
	else
		rc = mbedtls_pk_parse_key(&t->key, buf, off, NULL, 0, mbedtls_ctr_drbg_random, &t->rng);
out:
	if (buf) {
		mbedtls_platform_zeroize(buf, len + 1);
		free(buf);
	}
	close(fd);
	return rc == 0;
}

static bool load_pki(struct mesh_transport *t, const struct mesh_transport_options *o)
{
	static const unsigned char personalization[] = "meshd transport";
	const mbedtls_x509_crt *ca;
	uint32_t flags = 0;

#if defined(MBEDTLS_SSL_PROTO_TLS1_3) || defined(MBEDTLS_USE_PSA_CRYPTO)
	if (psa_crypto_init() != PSA_SUCCESS)
		return false;
#endif
	if (mbedtls_ctr_drbg_seed(&t->rng, mbedtls_entropy_func, &t->entropy,
				 personalization, sizeof(personalization) - 1) ||
	    !parse_pki_file(t, o->ca_file, &t->ca) || !parse_pki_file(t, o->cert_file, &t->local_cert) ||
	    !parse_pki_file(t, o->key_file, NULL))
		return false;
	for (ca = &t->ca; ca; ca = ca->next)
		if (mbedtls_x509_crt_get_ca_istrue(ca) != 1)
			return false;
	return leaf_valid_for(&t->local_cert, t->gateway) &&
	       mbedtls_x509_crt_verify(&t->local_cert, &t->ca, NULL, NULL, &flags, NULL, NULL) == 0 &&
	       flags == 0 && mbedtls_pk_check_pair(&t->local_cert.pk, &t->key,
						  mbedtls_ctr_drbg_random, &t->rng) == 0 &&
	       cert_id(&t->local_cert, t->local_id);
}

static bool configure_tls(struct mesh_transport *t)
{
	if (mbedtls_ssl_config_defaults(&t->tls, t->gateway ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
					MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT))
		return false;
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
	mbedtls_ssl_conf_min_tls_version(&t->tls, MBEDTLS_SSL_VERSION_TLS1_2);
#else
	mbedtls_ssl_conf_min_tls_version(&t->tls, MBEDTLS_SSL_VERSION_TLS1_3);
#endif
	mbedtls_ssl_conf_ciphersuites(&t->tls, tls_suites);
	mbedtls_ssl_conf_rng(&t->tls, mbedtls_ctr_drbg_random, &t->rng);
	mbedtls_ssl_conf_authmode(&t->tls, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&t->tls, &t->ca, NULL);
	if (!t->gateway)
		mbedtls_ssl_conf_verify(&t->tls, verify_gateway_pin, t);
	/* No session cache or ticket callbacks; node never imports a session. */
#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_CLI_C)
	if (!t->gateway)
		mbedtls_ssl_conf_session_tickets(&t->tls, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif
#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_SRV_C)
	if (t->gateway) {
		mbedtls_ssl_conf_session_tickets_cb(&t->tls, NULL, NULL, NULL);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
		mbedtls_ssl_conf_new_session_tickets(&t->tls, 0);
#endif
	}
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
	mbedtls_ssl_conf_tls13_key_exchange_modes(&t->tls, MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL);
#endif
#if defined(MBEDTLS_SSL_EARLY_DATA)
	mbedtls_ssl_conf_early_data(&t->tls, MBEDTLS_SSL_EARLY_DATA_DISABLED);
#endif
#if defined(MBEDTLS_SSL_RENEGOTIATION)
	mbedtls_ssl_conf_renegotiation(&t->tls, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
#endif
	return mbedtls_ssl_conf_own_cert(&t->tls, &t->local_cert, &t->key) == 0;
}

static bool prepare_discovery(struct mesh_transport *t, const struct mesh_transport_options *o)
{
	unsigned char digest[MESH_SHA256_LEN];
	size_t model_len, network_len;

	if (!o->model || !o->network)
		return false;
	model_len = strnlen(o->model, MESH_MODEL_MAX + 1);
	network_len = strnlen(o->network, MESH_NETWORK_MAX + 1);
	if (!network_len || network_len > MESH_NETWORK_MAX ||
	    !model_valid((const unsigned char *)o->model, model_len) ||
	    mbedtls_sha256((const unsigned char *)o->network, network_len, digest, 0))
		return false;
	memcpy(t->announcement, discovery_magic, sizeof(discovery_magic));
	t->announcement[DISCOVERY_ROLE_OFFSET] = t->gateway ? DISCOVERY_GATEWAY : DISCOVERY_CANDIDATE;
	memcpy(t->announcement + DISCOVERY_TAG_OFFSET, digest, MESH_NETWORK_TAG_LEN);
	memcpy(t->announcement + DISCOVERY_ID_OFFSET, t->local_id, sizeof(t->local_id));
	t->announcement[DISCOVERY_SIZE_OFFSET] = (unsigned char)model_len;
	memcpy(t->announcement + DISCOVERY_HEADER, o->model, model_len);
	t->announcement_len = DISCOVERY_HEADER + model_len;
	return true;
}

static bool start_udp(struct mesh_transport *t)
{
	bool receive = t->gateway && t->ops.discovered;

	t->udp_local.owner = t;
	if (udp_socket(&t->udp_local, t->management.sin_addr, true, receive))
		return false;
	if (receive) {
		t->udp_broadcast.owner = t;
		/* Linux delivers broadcast to a socket bound to the broadcast
		 * destination, not the socket bound to our unicast address.
		 */
		if (udp_socket(&t->udp_broadcast, t->broadcast.sin_addr, false, true) ||
		    uloop_fd_add(&t->udp_local.io, ULOOP_READ) ||
		    uloop_fd_add(&t->udp_broadcast.io, ULOOP_READ))
			return false;
	}
	return uloop_timeout_set(&t->announce, 1) == 0;
}

static bool start_listener(struct mesh_transport *t)
{
	struct sockaddr_in local = t->management;
	int one = 1;

	local.sin_port = htons(MESH_HEARTBEAT_PORT);
	t->listener.fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	return t->listener.fd >= 0 && bind_interface(t->listener.fd, t->ifindex) == 0 &&
		setsockopt(t->listener.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0 &&
		bind(t->listener.fd, (const struct sockaddr *)&local, sizeof(local)) == 0 &&
		listen(t->listener.fd, MESH_CONNECTION_MAX) == 0 &&
		uloop_fd_add(&t->listener, ULOOP_READ) == 0;
}

struct mesh_transport *mesh_transport_start(const struct mesh_transport_options *o,
					     const struct mesh_transport_ops *ops,
					     void *arg, char *error, size_t error_len)
{
	const char *failure = "invalid transport options or callbacks";
	struct mesh_transport *t = NULL;
	unsigned int i, count;
	bool advertise;

	if (error && error_len)
		*error = '\0';
	if (!o || !ops || (o->gateway && o->candidate) || !nonempty(o->cert_file) ||
	    !nonempty(o->key_file) || !nonempty(o->ca_file) ||
	    (o->gateway && (!ops->is_allowed || !ops->gateway_request)) ||
	    (!o->gateway && !o->candidate && (!ops->node_request || !ops->node_response)))
		goto fail;
	advertise = o->gateway || o->candidate;
	count = o->gateway ? MESH_CONNECTION_MAX : !o->candidate;
	failure = "cannot allocate transport";
	t = calloc(1, sizeof(*t) + count * sizeof(t->connections[0]));
	if (!t)
		goto fail;
	t->ops = *ops;
	t->arg = arg;
	t->gateway = o->gateway;
	t->candidate = o->candidate;
	t->connection_limit = count;
	t->udp_local.io.fd = -1;
	t->udp_local.io.cb = discovery_cb;
	t->udp_broadcast.io.fd = -1;
	t->udp_broadcast.io.cb = discovery_cb;
	t->listener.fd = -1;
	t->listener.cb = accept_cb;
	t->announce.cb = announce_cb;
	t->heartbeat.cb = heartbeat_cb;
	for (i = 0; i < count; i++) {
		struct mesh_connection *c = &t->connections[i];

		c->owner = t;
		c->io.fd = -1;
		c->io.cb = connection_cb;
		c->deadline.cb = connection_timeout;
	}
	mbedtls_entropy_init(&t->entropy);
	mbedtls_ctr_drbg_init(&t->rng);
	mbedtls_x509_crt_init(&t->ca);
	mbedtls_x509_crt_init(&t->local_cert);
	mbedtls_pk_init(&t->key);
	mbedtls_ssl_config_init(&t->tls);
	t->management.sin_family = AF_INET;
	t->broadcast.sin_family = AF_INET;
	t->broadcast.sin_port = htons(MESH_DISCOVERY_PORT);
	t->gateway_address.sin_family = AF_INET;
	t->gateway_address.sin_port = htons(MESH_HEARTBEAT_PORT);
	failure = "management address or broadcast address is not configured on a local interface";
	if (!parse_unicast(o->bind_ip, &t->management.sin_addr) ||
	    (advertise && (!o->broadcast_ip || inet_pton(AF_INET, o->broadcast_ip, &t->broadcast.sin_addr) != 1 ||
			   !t->broadcast.sin_addr.s_addr || t->broadcast.sin_addr.s_addr == t->management.sin_addr.s_addr)))
		goto fail;
	t->ifindex = management_ifindex(t->management.sin_addr, advertise ? &t->broadcast.sin_addr : NULL);
	if (!t->ifindex)
		goto fail;
	failure = "node requires a distinct gateway IPv4 address and a pinned leaf fingerprint";
	if (!advertise && (!parse_unicast(o->gateway_ip, &t->gateway_address.sin_addr) ||
	    t->gateway_address.sin_addr.s_addr == t->management.sin_addr.s_addr ||
	    !hex_to_id(o->gateway_id, t->gateway_id)))
		goto fail;
	failure = "local certificate, private key or private CA validation failed";
	if (!load_pki(t, o))
		goto fail;
	if (!advertise && memcmp(t->local_id, t->gateway_id, sizeof(t->local_id)) == 0) {
		failure = "gateway fingerprint must differ from the local leaf fingerprint";
		goto fail;
	}
	failure = "invalid discovery network name or model";
	if (advertise && !prepare_discovery(t, o))
		goto fail;
	failure = "cannot configure required mutual TLS";
	if (!t->candidate && !configure_tls(t))
		goto fail;
	failure = "cannot bind discovery UDP port on the configured management address and broadcast";
	if (advertise && !start_udp(t))
		goto fail;
	failure = "cannot listen on TCP heartbeat port on the configured management address";
	if (o->gateway && !start_listener(t))
		goto fail;
	if (!advertise) {
		unsigned char jitter;

		failure = "cannot schedule the first secure heartbeat";
		if (mbedtls_ctr_drbg_random(&t->rng, &jitter, sizeof(jitter)) ||
		    uloop_timeout_set(&t->heartbeat, 1 + 4 * jitter))
			goto fail;
	}
	return t;
fail:
	if (t)
		destroy_transport(t);
	if (error && error_len)
		(void)snprintf(error, error_len, "%s", failure);
	return NULL;
}

void mesh_transport_stop(struct mesh_transport *t)
{
	if (!t || t->stopping)
		return;
	t->stopping = true;
	if (!t->dispatch_depth)
		destroy_transport(t);
}

int mesh_transport_fingerprint(struct mesh_transport *t, char out[65])
{
	if (!t || t->stopping || !out)
		return -1;
	id_to_hex(t->local_id, out);
	return 0;
}

int mesh_transport_certificate(struct mesh_transport *t, const void **der, size_t *der_len)
{
	if (!t || t->stopping || !der || !der_len)
		return -1;
	*der = t->local_cert.raw.p;
	*der_len = t->local_cert.raw.len;
	return 0;
}

int mesh_transport_sign(struct mesh_transport *t, const void *data, size_t len,
			unsigned char *sig, size_t *sig_len)
{
	unsigned char hash[MESH_SHA256_LEN];
	size_t capacity, written = 0;
	int rc;

	if (!sig_len)
		return -1;
	capacity = *sig_len;
	*sig_len = 0;
	if (!t || t->stopping || (!data && len) || !sig || !capacity)
		return -1;
	rc = mbedtls_sha256(data ? data : "", len, hash, 0);
	if (!rc)
		rc = mbedtls_pk_sign(&t->key, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, capacity,
				     &written, mbedtls_ctr_drbg_random, &t->rng);
	if (rc)
		return rc;
	*sig_len = written;
	return 0;
}

int mesh_transport_verify(const void *der, size_t der_len, const void *data, size_t len,
			  const unsigned char *sig, size_t sig_len)
{
	unsigned char hash[MESH_SHA256_LEN];
	mbedtls_x509_crt cert;
	int rc;

	if (!der || !der_len || (!data && len) || !sig || !sig_len ||
	    sig_len > MBEDTLS_PK_SIGNATURE_MAX_SIZE)
		return -1;
#if defined(MBEDTLS_USE_PSA_CRYPTO)
	if (psa_crypto_init() != PSA_SUCCESS)
		return -1;
#endif
	mbedtls_x509_crt_init(&cert);
	rc = mbedtls_x509_crt_parse_der_nocopy(&cert, der, der_len);
	if (!rc && (cert.raw.len != der_len || !leaf_valid_for(&cert, true)))
		rc = -1;
	if (!rc)
		rc = mbedtls_sha256(data ? data : "", len, hash, 0);
	if (!rc)
		rc = mbedtls_pk_verify(&cert.pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sig_len);
	mbedtls_x509_crt_free(&cert);
	return rc;
}
