/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MESH_TRANSPORT_H
#define MESH_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

#define MESH_FRAME_MAX 2048

struct mesh_transport;

struct mesh_transport_options {
	const char *bind_ip;
	const char *broadcast_ip;
	const char *gateway_ip;
	const char *gateway_id; /* SHA-256 of the configured gateway's leaf DER, 64 hex digits. */
	const char *network;
	const char *model;
	const char *cert_file;
	const char *key_file;
	const char *ca_file;
	bool gateway;
	bool candidate; /* UDP announcements only; mutually exclusive with gateway. */
};

/* Callbacks run on the uloop thread. Their inputs are borrowed for the call.
 * discovered is an unauthenticated hint, never an enrollment or change request.
 * is_allowed must explicitly allow the verified node leaf fingerprint. It can
 * be called multiple times for a connection and must not have side effects.
 * The gateway_request IPv4 address is the actual authenticated connection's
 * peer address. Return 0 (or more than capacity) to send no frame.
 */
struct mesh_transport_ops {
	void (*discovered)(void *arg, const char *id, const char *ip, const char *model);
	bool (*is_allowed)(void *arg, const char *id);
	size_t (*node_request)(void *arg, unsigned char *out, size_t capacity);
	size_t (*gateway_request)(void *arg, const char *id, const char *ip,
				  const unsigned char *request, size_t request_len,
				  unsigned char *out, size_t capacity);
	void (*node_response)(void *arg, const unsigned char *response, size_t len,
			      const void *gateway_der, size_t der_len);
};

/* Call after uloop_init(), from the uloop thread. Options are consumed by
 * start; the ops are copied. Keep arg valid until stop. Callbacks must not
 * recursively run uloop; they may stop this transport.
 * bind_ip must be an explicit local IPv4 address. A node needs a configured
 * gateway_ip and gateway_id. A gateway or candidate needs the management
 * interface's broadcast_ip (or 255.255.255.255), a nonempty network name of
 * at most 128 bytes and a model of 1 to 63 printable ASCII bytes.
 * All modes require a local leaf/key pair and the private CA trust file.
 * Use regular root-owned files under a protected directory, with private
 * key mode 0600 (or 0400) and no non-owner write access to any file.
 */
struct mesh_transport *mesh_transport_start(const struct mesh_transport_options *o,
					     const struct mesh_transport_ops *ops,
					     void *arg, char *error, size_t error_len);
void mesh_transport_stop(struct mesh_transport *t);

/* 0 on success, negative on error. Fingerprints are lowercase hex. */
int mesh_transport_fingerprint(struct mesh_transport *t, char out[65]);
/* Borrow the local leaf DER until the transport is stopped. */
int mesh_transport_certificate(struct mesh_transport *t, const void **der, size_t *der_len);
/* sig_len is the input capacity, and becomes the signature length (0 on error). */
int mesh_transport_sign(struct mesh_transport *t, const void *data, size_t len,
			unsigned char *sig, size_t *sig_len);
/* Only use DER from the already authenticated gateway (node_response).
 * This verifies a SHA-256 application signature, not a certificate's trust.
 */
int mesh_transport_verify(const void *der, size_t der_len, const void *data, size_t len,
			  const unsigned char *sig, size_t sig_len);

#endif
