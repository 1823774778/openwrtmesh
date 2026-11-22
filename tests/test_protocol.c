/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct mesh_profile profile(void)
{
	return (struct mesh_profile){ .version = 0x01020304, .flags = MESH_K_NEIGHBOR | MESH_FT,
		.ssid = "\xe5\xae\xb6 Wi-Fi #1", .key = "example-1234's=key" };
}

static void validation(void)
{
	struct mesh_profile p = profile();
	char id[65];

	memset(id, 'a', 64); id[64] = 0;
	assert(mesh_id_valid(id));
	id[0] = 'A'; assert(!mesh_id_valid(id));
	id[0] = 'z'; assert(!mesh_id_valid(id));
	id[0] = 0; assert(!mesh_id_valid(id));
	assert(mesh_model_valid("OpenWrt Gateway"));
	assert(!mesh_model_valid("OpenWrt\n"));
	assert(!mesh_model_valid(""));
	assert(mesh_profile_valid(&p));
	p.version = 0; assert(!mesh_profile_valid(&p));
	p = profile(); p.flags = 16; assert(!mesh_profile_valid(&p));
	p = profile(); strcpy(p.ssid, "\xf0\x9f\x8f\xa0"); assert(mesh_profile_valid(&p));
	strcpy(p.ssid, "\xed\xa0\x80"); assert(!mesh_profile_valid(&p));
	strcpy(p.ssid, "\xc0\xaf"); assert(!mesh_profile_valid(&p));
	strcpy(p.ssid, "\xc2\x85"); assert(!mesh_profile_valid(&p));
	strcpy(p.ssid, "\xf4\x90\x80\x80"); assert(!mesh_profile_valid(&p));
	strcpy(p.ssid, "\xe5\xae"); assert(!mesh_profile_valid(&p));
	strcpy(p.ssid, "home\nssid"); assert(!mesh_profile_valid(&p));
	p = profile(); memset(p.ssid, 'a', sizeof(p.ssid)); assert(!mesh_profile_valid(&p));
	p.ssid[32] = 0; assert(mesh_profile_valid(&p));
	strcpy(p.key, "short"); assert(!mesh_profile_valid(&p));
	strcpy(p.key, "12345678"); assert(mesh_profile_valid(&p));
	strcpy(p.key, "\xc3\xa9" "12345678"); assert(!mesh_profile_valid(&p));
	memset(p.key, 'F', 64); p.key[64] = 0; assert(mesh_profile_valid(&p));
	p.key[60] = 'X'; assert(!mesh_profile_valid(&p));
	p.key[63] = 0; assert(mesh_profile_valid(&p));
}

static void signed_frame(void)
{
	const unsigned char scope[MESH_SCOPE_LEN] = { 42 }, wrong[MESH_SCOPE_LEN] = { 41 };
	const unsigned char signature[] = { 1, 2, 3, 4 };
	unsigned char wire[MESH_SIGNED_MAX + 1] = { 0 };
	struct mesh_profile p = profile(), decoded;
	struct mesh_signature_view sig;
	size_t payload = mesh_payload_encode(&p, scope, wire, sizeof(wire));

	assert(payload && !mesh_payload_encode(&p, scope, wire, payload - 1));
	assert(!memcmp(wire, "MCF\1\1\2\3\4", 8));
	assert(!mesh_signed_append(wire, payload, sizeof(wire), signature, 0));
	assert(!mesh_signed_append(wire, payload, payload + 2, signature, sizeof(signature)));
	size_t n = mesh_signed_append(wire, payload, sizeof(wire), signature, sizeof(signature));
	assert(n == payload + 2 + sizeof(signature));
	assert(mesh_signed_decode(wire, n, scope, &decoded, &sig));
	assert(decoded.version == p.version && decoded.flags == p.flags);
	assert(!strcmp(decoded.ssid, p.ssid) && !strcmp(decoded.key, p.key));
	assert(sig.payload_len == payload && sig.signature_len == sizeof(signature));
	assert(!memcmp(sig.signature, signature, sizeof(signature)));
	for (size_t i = 0; i < n; i++)
		assert(!mesh_signed_decode(wire, i, scope, &decoded, &sig));
	assert(!mesh_signed_decode(wire, n + 1, scope, &decoded, &sig));
	assert(!mesh_signed_decode(wire, n, wrong, &decoded, &sig));
	wire[8] |= 64; assert(!mesh_signed_decode(wire, n, scope, &decoded, &sig)); wire[8] &= ~64;
	wire[9] = 0; assert(!mesh_signed_decode(wire, n, scope, &decoded, &sig)); wire[9] = strlen(p.ssid);
	wire[11 + MESH_SCOPE_LEN] = 0; assert(!mesh_signed_decode(wire, n, scope, &decoded, &sig));
}

static void heartbeat(void)
{
	const unsigned char scope[MESH_SCOPE_LEN] = { 77 }, wrong[MESH_SCOPE_LEN] = { 78 };
	struct mesh_heartbeat h = { .version = 32, .rejected_version = 90, .drift = true, .model = "Test" }, got;
	unsigned char wire[MESH_HEARTBEAT_MAX + 1] = { 0 };
	size_t n = mesh_heartbeat_encode(&h, scope, wire, sizeof(wire));

	assert(n && mesh_heartbeat_decode(wire, n, scope, &got));
	assert(got.version == 32 && got.rejected_version == 90 && got.drift && !strcmp(got.model, "Test"));
	for (size_t i = 0; i < n; i++)
		assert(!mesh_heartbeat_decode(wire, i, scope, &got));
	assert(!mesh_heartbeat_decode(wire, n + 1, scope, &got));
	assert(!mesh_heartbeat_decode(wire, n, wrong, &got));
	wire[12] = 2; assert(!mesh_heartbeat_decode(wire, n, scope, &got));
}

int main(void)
{
	validation();
	signed_frame();
	heartbeat();
	puts("protocol: validation, signed envelope and heartbeat tests passed");
	return 0;
}
