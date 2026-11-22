/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MESH_PROTOCOL_H
#define MESH_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESH_ID_LEN 64
#define MESH_MODEL_LEN 64
#define MESH_SCOPE_LEN 32
#define MESH_SIGNATURE_MAX 512
#define MESH_PAYLOAD_MAX (11 + MESH_SCOPE_LEN + 32 + 64)
#define MESH_SIGNED_MAX (MESH_PAYLOAD_MAX + 2 + MESH_SIGNATURE_MAX)
#define MESH_HEARTBEAT_MAX (14 + 2 * MESH_SCOPE_LEN + MESH_MODEL_LEN - 1)
#define MESH_ACK_LEN 4

enum mesh_flags {
	MESH_K_NEIGHBOR = 1,
	MESH_K_BEACON = 2,
	MESH_V_BTM = 4,
	MESH_FT = 8,
	MESH_FLAGS_ALL = 15,
};

struct mesh_profile {
	uint32_t version;
	unsigned int flags;
	char ssid[33];
	char key[65];
};

struct mesh_heartbeat {
	uint32_t version;
	uint32_t rejected_version;
	bool drift;
	unsigned char profile_hash[MESH_SCOPE_LEN];
	char model[MESH_MODEL_LEN];
};

struct mesh_signature_view {
	const unsigned char *bytes;
	size_t payload_len;
	const unsigned char *signature;
	size_t signature_len;
};

extern const unsigned char mesh_ack[MESH_ACK_LEN];

bool mesh_id_valid(const char *s);
bool mesh_model_valid(const char *s);
bool mesh_profile_valid(const struct mesh_profile *p);
size_t mesh_payload_encode(const struct mesh_profile *p, const unsigned char scope[MESH_SCOPE_LEN],
			   unsigned char *out, size_t capacity);
size_t mesh_signed_append(unsigned char *buf, size_t payload_len, size_t capacity,
			  const unsigned char *sig, size_t sig_len);
bool mesh_signed_decode(const unsigned char *buf, size_t len, const unsigned char scope[MESH_SCOPE_LEN],
			struct mesh_profile *p, struct mesh_signature_view *signature);
size_t mesh_heartbeat_encode(const struct mesh_heartbeat *hb, const unsigned char scope[MESH_SCOPE_LEN],
			     unsigned char *out, size_t capacity);
bool mesh_heartbeat_decode(const unsigned char *in, size_t len, const unsigned char scope[MESH_SCOPE_LEN],
			   struct mesh_heartbeat *hb);

#endif
