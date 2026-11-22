/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "protocol.h"

#include <string.h>

const unsigned char mesh_ack[MESH_ACK_LEN] = { 'M', 'A', 'K', 1 };
static const unsigned char heartbeat_magic[] = { 'M', 'H', 'B', 1 };
static const unsigned char profile_magic[] = { 'M', 'C', 'F', 1 };

enum { PAYLOAD_HEADER = 11 + MESH_SCOPE_LEN, HEARTBEAT_HEADER = 14 + 2 * MESH_SCOPE_LEN };

static bool hexchar(unsigned char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static uint32_t get32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

bool mesh_id_valid(const char *s)
{
	if (!s || strnlen(s, MESH_ID_LEN + 1) != MESH_ID_LEN)
		return false;
	for (; *s; s++) {
		if (!hexchar(*s) || (*s >= 'A' && *s <= 'F'))
			return false;
	}
	return true;
}

bool mesh_model_valid(const char *s)
{
	size_t len = s ? strnlen(s, MESH_MODEL_LEN) : 0;

	if (len == 0 || len >= MESH_MODEL_LEN)
		return false;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = s[i];
		if (c < 32 || c > 126)
			return false;
	}
	return true;
}

/* 接受合法 UTF-8 字节；拒绝空字节、控制符、代理项和过长编码。 */
static bool ssid_valid(const unsigned char *s, size_t n)
{
	for (size_t i = 0; i < n; ) {
		uint32_t cp = s[i++], min;
		unsigned int left;

		if (cp < 0x80) {
			if (cp < 32 || cp == 127)
				return false;
			continue;
		}
		if (cp >= 0xc2 && cp <= 0xdf) {
			cp &= 0x1f; left = 1; min = 0x80;
		} else if (cp >= 0xe0 && cp <= 0xef) {
			cp &= 0x0f; left = 2; min = 0x800;
		} else if (cp >= 0xf0 && cp <= 0xf4) {
			cp &= 0x07; left = 3; min = 0x10000;
		} else {
			return false;
		}
		if (n - i < left)
			return false;
		while (left--) {
			if ((s[i] & 0xc0) != 0x80)
				return false;
			cp = (cp << 6) | (s[i++] & 0x3f);
		}
		if (cp < min || cp <= 0x9f || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
			return false;
	}
	return true;
}

bool mesh_profile_valid(const struct mesh_profile *p)
{
	size_t ssid_len, key_len;

	if (!p || !p->version || p->flags & ~MESH_FLAGS_ALL)
		return false;
	ssid_len = strnlen(p->ssid, sizeof(p->ssid));
	key_len = strnlen(p->key, sizeof(p->key));
	if (!ssid_len || ssid_len >= sizeof(p->ssid) || key_len < 8 || key_len >= sizeof(p->key) ||
	    !ssid_valid((const unsigned char *)p->ssid, ssid_len))
		return false;
	for (size_t i = 0; i < key_len; i++) {
		unsigned char c = p->key[i];
		if ((key_len == 64 && !hexchar(c)) || c < 32 || c > 126)
			return false;
	}
	return true;
}

size_t mesh_payload_encode(const struct mesh_profile *p, const unsigned char scope[MESH_SCOPE_LEN],
			   unsigned char *out, size_t capacity)
{
	size_t s, k;

	if (!scope || !out || !mesh_profile_valid(p))
		return 0;
	s = strlen(p->ssid);
	k = strlen(p->key);
	if (capacity < PAYLOAD_HEADER + s + k)
		return 0;
	memcpy(out, profile_magic, sizeof(profile_magic));
	put32(out + 4, p->version);
	out[8] = p->flags;
	out[9] = s;
	out[10] = k;
	memcpy(out + 11, scope, MESH_SCOPE_LEN);
	memcpy(out + PAYLOAD_HEADER, p->ssid, s);
	memcpy(out + PAYLOAD_HEADER + s, p->key, k);
	return PAYLOAD_HEADER + s + k;
}

size_t mesh_signed_append(unsigned char *buf, size_t payload_len, size_t capacity,
			  const unsigned char *sig, size_t sig_len)
{
	if (!buf || !sig || !sig_len || sig_len > MESH_SIGNATURE_MAX ||
	    payload_len < PAYLOAD_HEADER || payload_len > MESH_PAYLOAD_MAX ||
	    capacity < payload_len + 2 + sig_len)
		return 0;
	buf[payload_len] = sig_len >> 8;
	buf[payload_len + 1] = sig_len;
	memcpy(buf + payload_len + 2, sig, sig_len);
	return payload_len + 2 + sig_len;
}

bool mesh_signed_decode(const unsigned char *buf, size_t len, const unsigned char scope[MESH_SCOPE_LEN],
			struct mesh_profile *p, struct mesh_signature_view *signature)
{
	struct mesh_profile candidate = { 0 };
	size_t s, k, payload, sig;

	if (!buf || !scope || !p || !signature || len < PAYLOAD_HEADER + 2 || len > MESH_SIGNED_MAX ||
	    memcmp(buf, profile_magic, sizeof(profile_magic)) || memcmp(buf + 11, scope, MESH_SCOPE_LEN))
		return false;
	s = buf[9];
	k = buf[10];
	payload = PAYLOAD_HEADER + s + k;
	if (!s || s > 32 || k < 8 || k > 64 || len < payload + 2 ||
	    memchr(buf + PAYLOAD_HEADER, '\0', s + k))
		return false;
	sig = (size_t)buf[payload] << 8 | buf[payload + 1];
	if (!sig || sig > MESH_SIGNATURE_MAX || len != payload + 2 + sig)
		return false;
	candidate.version = get32(buf + 4);
	candidate.flags = buf[8];
	memcpy(candidate.ssid, buf + PAYLOAD_HEADER, s);
	memcpy(candidate.key, buf + PAYLOAD_HEADER + s, k);
	if (!mesh_profile_valid(&candidate))
		return false;
	*p = candidate;
	*signature = (struct mesh_signature_view){ buf, payload, buf + payload + 2, sig };
	return true;
}

size_t mesh_heartbeat_encode(const struct mesh_heartbeat *hb, const unsigned char scope[MESH_SCOPE_LEN],
			     unsigned char *out, size_t capacity)
{
	size_t n;

	if (!hb || !scope || !out || !mesh_model_valid(hb->model))
		return 0;
	n = strlen(hb->model);
	if (capacity < HEARTBEAT_HEADER + n)
		return 0;
	memcpy(out, heartbeat_magic, sizeof(heartbeat_magic));
	put32(out + 4, hb->version);
	put32(out + 8, hb->rejected_version);
	out[12] = hb->drift;
	out[13] = n;
	memcpy(out + 14, scope, MESH_SCOPE_LEN);
	memcpy(out + 14 + MESH_SCOPE_LEN, hb->profile_hash, MESH_SCOPE_LEN);
	memcpy(out + HEARTBEAT_HEADER, hb->model, n);
	return HEARTBEAT_HEADER + n;
}

bool mesh_heartbeat_decode(const unsigned char *in, size_t len, const unsigned char scope[MESH_SCOPE_LEN],
			   struct mesh_heartbeat *hb)
{
	struct mesh_heartbeat candidate = { 0 };
	size_t n;

	if (!in || !scope || !hb || len < HEARTBEAT_HEADER || len > MESH_HEARTBEAT_MAX ||
	    memcmp(in, heartbeat_magic, sizeof(heartbeat_magic)) || in[12] > 1 ||
	    memcmp(in + 14, scope, MESH_SCOPE_LEN))
		return false;
	n = in[13];
	if (!n || n >= MESH_MODEL_LEN || len != HEARTBEAT_HEADER + n ||
	    memchr(in + HEARTBEAT_HEADER, '\0', n))
		return false;
	memcpy(candidate.model, in + HEARTBEAT_HEADER, n);
	if (!mesh_model_valid(candidate.model))
		return false;
	candidate.version = get32(in + 4);
	candidate.rejected_version = get32(in + 8);
	candidate.drift = in[12] != 0;
	memcpy(candidate.profile_hash, in + 14 + MESH_SCOPE_LEN, MESH_SCOPE_LEN);
	*hb = candidate;
	return true;
}
