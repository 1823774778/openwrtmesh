/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Build (OpenWrt/Linux with development headers):
 * cc -std=gnu11 -Wall -Wextra -Werror -o test_transport \
 *   package/meshd/tests/test_transport.c package/meshd/src/transport.c \
 *   -lubox -lmbedtls -lmbedx509 -lmbedcrypto
 * Run in an isolated network namespace with two IPv4 addresses configured on
 * the same interface (for example 127.0.0.1 and an explicitly added 127.0.0.2
 * on an up lo). Supply an offline test CA, distinct serverAuth/clientAuth
 * leaf/key pairs signed by that CA, and the two addresses. Use root-owned
 * fixtures with keys mode 0600 in the temporary VM or network namespace.
 * The broadcast defaults to 255.255.255.255; pass a directed broadcast for a test interface
 * if needed. Pass `tls-only` last to omit the UDP delivery assertion.
 */
#include "../src/transport.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/uloop.h>

struct fixture {
	struct mesh_transport *gateway;
	struct mesh_transport *node;
	const char *node_ip;
	char node_id[65];
	bool allow;
	bool silent;
	bool stop_on_response;
	unsigned int allow_checks;
	unsigned int node_requests;
	unsigned int gateway_requests;
	unsigned int node_responses;
	unsigned int discoveries;
};

static const unsigned char message[] = { 'o', 'k', 0, 0xfe };

static void discovered(void *arg, const char *id, const char *ip, const char *model)
{
	struct fixture *f = arg;

	assert(!strcmp(f->node_id, id));
	assert(!strcmp(f->node_ip, ip));
	assert(!strcmp("test-node", model));
	f->discoveries++;
}

static bool is_allowed(void *arg, const char *id)
{
	struct fixture *f = arg;

	assert(!strcmp(f->node_id, id));
	f->allow_checks++;
	return f->allow;
}

static size_t node_request(void *arg, unsigned char *out, size_t capacity)
{
	struct fixture *f = arg;
	size_t i;

	f->node_requests++;
	assert(capacity == MESH_FRAME_MAX);
	for (i = 0; i < capacity; i++)
		out[i] = (unsigned char)i;
	return capacity;
}

static size_t gateway_request(void *arg, const char *id, const char *ip,
			      const unsigned char *request, size_t request_len,
			      unsigned char *out, size_t capacity)
{
	struct fixture *f = arg;
	size_t i, sig_len;

	f->gateway_requests++;
	assert(f->allow && f->allow_checks >= 2);
	assert(!strcmp(f->node_id, id));
	assert(!strcmp(f->node_ip, ip));
	assert(request_len == MESH_FRAME_MAX);
	for (i = 0; i < request_len; i++)
		assert(request[i] == (unsigned char)i);
	if (f->silent)
		return 0;
	assert(capacity > sizeof(message));
	memcpy(out, message, sizeof(message));
	sig_len = capacity - sizeof(message);
	assert(mesh_transport_sign(f->gateway, message, sizeof(message), out + sizeof(message), &sig_len) == 0);
	return sizeof(message) + sig_len;
}

static void node_response(void *arg, const unsigned char *response, size_t len,
			  const void *gateway_der, size_t der_len)
{
	struct fixture *f = arg;
	const void *expected;
	size_t expected_len, sig_len;

	f->node_responses++;
	assert(len > sizeof(message));
	assert(!memcmp(message, response, sizeof(message)));
	assert(mesh_transport_certificate(f->gateway, &expected, &expected_len) == 0);
	assert(der_len == expected_len && !memcmp(expected, gateway_der, der_len));
	sig_len = len - sizeof(message);
	assert(mesh_transport_verify(gateway_der, der_len, message, sizeof(message),
				     response + sizeof(message), sig_len) == 0);
	assert(mesh_transport_verify(gateway_der, der_len, message, sizeof(message) - 1,
				     response + sizeof(message), sig_len) != 0);
	if (f->stop_on_response) {
		mesh_transport_stop(f->node);
		f->node = NULL;
		uloop_end();
	}
}

static void stop_loop(struct uloop_timeout *timer)
{
	(void)timer;
	uloop_end();
}

static void run_for(int msecs)
{
	struct uloop_timeout stop = { .cb = stop_loop };

	assert(uloop_timeout_set(&stop, msecs) == 0);
	uloop_run();
	if (stop.pending)
		uloop_timeout_cancel(&stop);
}

static struct mesh_transport *start(const struct mesh_transport_options *o,
				    const struct mesh_transport_ops *ops, struct fixture *f)
{
	char error[256];
	struct mesh_transport *t = mesh_transport_start(o, ops, f, error, sizeof(error));

	if (!t) {
		fprintf(stderr, "transport startup failed: %s\n", error);
		exit(1);
	}
	return t;
}

int main(int argc, char *argv[])
{
	const struct mesh_transport_ops ops = {
		.discovered = discovered,
		.is_allowed = is_allowed,
		.gateway_request = gateway_request,
		.node_request = node_request,
		.node_response = node_response,
	};
	struct mesh_transport_options gateway, node, invalid;
	struct fixture f = { 0 };
	struct mesh_transport *candidate;
	char gateway_id[65], wrong_id[65], error[256];
	unsigned char tiny[2] = { 0x36, 0x71 };
	size_t tiny_len = 1;
	bool expect_discovery;

	if (argc < 8 || argc > 10 || (argc == 10 && strcmp(argv[9], "tls-only"))) {
		fprintf(stderr, "usage: %s CA GATEWAY_CERT GATEWAY_KEY NODE_CERT NODE_KEY "
			"GATEWAY_IP NODE_IP [BROADCAST_IP [tls-only]]\n", argv[0]);
		return 2;
	}
	f.node_ip = argv[7];
	expect_discovery = argc != 10;
	gateway = (struct mesh_transport_options) {
		.bind_ip = argv[6],
		.broadcast_ip = argc >= 9 ? argv[8] : "255.255.255.255",
		.network = "mesh-transport-test",
		.model = "test-gateway",
		.cert_file = argv[2],
		.key_file = argv[3],
		.ca_file = argv[1],
		.gateway = true,
	};
	node = (struct mesh_transport_options) {
		.bind_ip = f.node_ip,
		.broadcast_ip = gateway.broadcast_ip,
		.gateway_ip = gateway.bind_ip,
		.gateway_id = gateway_id,
		.network = gateway.network,
		.model = "test-node",
		.cert_file = argv[4],
		.key_file = argv[5],
		.ca_file = gateway.ca_file,
		.candidate = true,
	};
	assert(uloop_init() == 0);
	invalid = gateway;
	invalid.bind_ip = "0.0.0.0";
	assert(mesh_transport_start(&invalid, &ops, &f, error, sizeof(error)) == NULL);
	invalid = gateway;
	invalid.ca_file = invalid.cert_file; /* A leaf cannot be a trust anchor. */
	assert(mesh_transport_start(&invalid, &ops, &f, error, sizeof(error)) == NULL);
	f.gateway = start(&gateway, &ops, &f);
	assert(mesh_transport_fingerprint(f.gateway, gateway_id) == 0);
	assert(mesh_transport_sign(f.gateway, message, sizeof(message), tiny, &tiny_len) != 0);
	assert(tiny_len == 0 && tiny[1] == 0x71);

	if (expect_discovery) {
		invalid = node;
		invalid.network = "different-cluster";
		candidate = start(&invalid, &ops, &f);
		assert(mesh_transport_fingerprint(candidate, f.node_id) == 0);
		run_for(200);
		assert(!f.discoveries);
		mesh_transport_stop(candidate);
	}
	candidate = start(&node, &ops, &f);
	assert(mesh_transport_fingerprint(candidate, f.node_id) == 0);
	assert(strcmp(gateway_id, f.node_id));
	if (expect_discovery) {
		run_for(600);
		assert(f.discoveries == 1);
	}
	assert(!f.node_requests && !f.allow_checks);
	mesh_transport_stop(candidate);
	node.candidate = false;

	memcpy(wrong_id, gateway_id, sizeof(wrong_id));
	wrong_id[0] = wrong_id[0] == '0' ? '1' : '0';
	invalid = node;
	invalid.gateway_id = wrong_id;
	f.allow = true;
	f.node = start(&invalid, &ops, &f);
	run_for(1800);
	assert(!f.node_requests && !f.allow_checks && !f.gateway_requests && !f.node_responses);
	mesh_transport_stop(f.node);

	f.allow = false;
	f.node = start(&node, &ops, &f);
	run_for(1800);
	assert(f.allow_checks && !f.gateway_requests && !f.node_responses);
	mesh_transport_stop(f.node);

	f.allow = true;
	f.silent = true;
	f.node = start(&node, &ops, &f);
	run_for(1800);
	assert(f.gateway_requests && !f.node_responses);
	mesh_transport_stop(f.node);

	f.silent = false;
	f.stop_on_response = true;
	f.node = start(&node, &ops, &f);
	run_for(4000);
	assert(f.node_responses == 1 && f.node == NULL);
	mesh_transport_stop(f.gateway);
	uloop_done();
	puts("transport integration tests passed");
	return 0;
}
