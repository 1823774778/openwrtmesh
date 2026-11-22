/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MESH_LOCAL_H
#define MESH_LOCAL_H

#include "protocol.h"
#include "wifi.h"

#define MESH_DATA "/etc/meshd"
#define MESH_PROFILE_FILE "profile.bin"
#define MESH_JOURNAL_FILE "transaction"
#define MESH_ALLOW_FILE "authorized"
#define MESH_FAILED_FILE "rejected"
#define MESH_NODES_MAX 10
#define MESH_CANDIDATES_MAX 16
#define MESH_CERT_MAX 2048
#define MESH_RECORD_MAX (2 + MESH_CERT_MAX + MESH_SIGNED_MAX)

struct mesh_settings {
	bool enabled;
	bool valid;
	char role[10];
	char network[129];
	char model[MESH_MODEL_LEN];
	char bind_ip[16];
	char broadcast_ip[16];
	char gateway_ip[16];
	char gateway_id[MESH_ID_LEN + 1];
	char backhaul[8];
	struct mesh_wifi wifi;
};

void mesh_load_settings(struct mesh_settings *s, char *error, size_t error_len);
/* 打开仅 root 可用的状态目录；读取返回 0/1/-1（成功/不存在/其他错误）。 */
int mesh_store_open(void);
int mesh_store_read(int dir, const char *name, void *data, size_t cap, size_t *size);
/* 写入返回 0/-1/-2（成功/未写入/已替换但无法确认持久化）。 */
int mesh_store_write(int dir, const char *name, const void *data, size_t len);
int mesh_store_remove(int dir, const char *name);
uint64_t mesh_now_ms(void);
unsigned int mesh_jitter(unsigned int max_ms);
void mesh_hash_hex(const void *data, size_t len, char out[MESH_ID_LEN + 1]);

#endif
