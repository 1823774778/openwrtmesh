/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MESH_CORE_H
#define MESH_CORE_H

#include "local.h"
#include "transport.h"

#include <libubus.h>

#define MESH_EVENT_MAX 128
#define MESH_ERROR_LEN 160

enum mesh_phase { PH_IDLE, PH_APPLYING, PH_VERIFYING, PH_COMMITTED, PH_ROLLING_BACK, PH_ROLLED_BACK, PH_FAILED };

struct mesh_peer {
	char id[65], ip[16], model[MESH_MODEL_LEN];
	uint64_t seen;
	uint32_t version, rejected;
	bool drift;
	bool digest_mismatch;
	unsigned int observed_state;
};

struct mesh_event {
	const char *type;
	char detail[MESH_ERROR_LEN];
	uint64_t time;
};

struct mesh_core {
	struct mesh_settings settings;
	struct ubus_context *ubus;
	struct mesh_transport *transport;
	int dir;
	char id[65];
	unsigned char scope[MESH_SCOPE_LEN], profile_hash[MESH_SCOPE_LEN];
	char error[MESH_ERROR_LEN], profile_error[MESH_ERROR_LEN], txn_error[MESH_ERROR_LEN];
	bool storage_blocked, drift, cancel, rollback_disk_done, rollback_marker;
	struct mesh_profile current, pending;
	unsigned char record[MESH_RECORD_MAX], staged[MESH_RECORD_MAX];
	size_t record_len, staged_len;
	uint32_t rejected, target_version;
	enum mesh_phase phase;
	uint64_t last_gateway, verify_since, deadline;
	struct uloop_timeout txn_tick, core_tick;
	struct uloop_process worker;
	uint64_t worker_deadline;
	struct mesh_peer peers[MESH_NODES_MAX], candidates[MESH_CANDIDATES_MAX];
	bool rollout_paused;
	char active_peer[65];
	uint64_t rollout_after, rollout_deadline;
	struct mesh_event events[MESH_EVENT_MAX];
	size_t event_count, event_next;
};

bool mesh_is_gateway(const struct mesh_core *m);
bool mesh_is_busy(const struct mesh_core *m);
const char *mesh_phase_name(enum mesh_phase phase);
const char *mesh_peer_state(const struct mesh_peer *p, uint64_t now);
size_t mesh_node_count(const struct mesh_core *m, bool converged_only);
const char *mesh_rollout_state(const struct mesh_core *m);
int mesh_allow_change(struct mesh_core *m, const char *id, bool allow);
int mesh_apply(struct mesh_core *m, struct mesh_profile *p);
int mesh_cancel(struct mesh_core *m);
void mesh_event(struct mesh_core *m, const char *type, const char *detail);
int mesh_rpc_start(struct mesh_core *m);

#endif
