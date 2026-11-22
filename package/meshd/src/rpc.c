/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core.h"

#include <mbedtls/platform_util.h>
#include <string.h>

#define CORE(obj) container_of(obj, struct mesh_rpc, obj)
#define RPC_ARGS struct ubus_context *u, struct ubus_object *obj, \
	struct ubus_request_data *req, const char *method, struct blob_attr *msg
#define UNUSED_REQUEST do { (void)method; (void)msg; } while (0)

struct mesh_rpc {
	struct ubus_object obj;
	struct mesh_core *core;
};

static int reply(struct ubus_context *u, struct ubus_request_data *req, struct blob_buf *b)
{
	int rc = b->head ? ubus_send_reply(u, req, b->head) : UBUS_STATUS_NO_MEMORY;

	blob_buf_free(b);
	return rc;
}

static int status(RPC_ARGS)
{
	struct mesh_core *m = CORE(obj)->core;
	struct blob_buf b = { 0 };
	uint64_t now = mesh_now_ms();

	UNUSED_REQUEST;
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "enabled", m->settings.enabled);
	blobmsg_add_string(&b, "role", m->settings.role);
	blobmsg_add_string(&b, "node_id", m->id);
	blobmsg_add_u8(&b, "transport", m->transport != NULL && !m->storage_blocked);
	blobmsg_add_u8(&b, "gateway_connected", m->last_gateway && now - m->last_gateway < 15000);
	blobmsg_add_string(&b, "error", m->error[0] ? m->error : m->profile_error);
	blobmsg_add_string(&b, "backhaul", m->settings.backhaul);
	blobmsg_add_u32(&b, "applied_version", m->current.version);
	return reply(u, req, &b);
}

static void add_node(struct blob_buf *b, const struct mesh_peer *p, const char *role, const char *state,
		     bool approved, bool failed)
{
	void *t = blobmsg_open_table(b, NULL);

	blobmsg_add_string(b, "id", p->id);
	blobmsg_add_string(b, "role", role);
	blobmsg_add_string(b, "ip", p->ip);
	blobmsg_add_string(b, "model", p->model);
	blobmsg_add_string(b, "state", state);
	blobmsg_add_u8(b, "approved", approved);
	blobmsg_add_u8(b, "config_failed", failed);
	blobmsg_add_u32(b, "version", p->version);
	blobmsg_close_table(b, t);
}

static int topology(RPC_ARGS)
{
	struct mesh_core *m = CORE(obj)->core;
	struct blob_buf b = { 0 };
	uint64_t now = mesh_now_ms();
	void *arr;

	UNUSED_REQUEST;
	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "nodes");
	if (mesh_is_gateway(m)) {
		for (size_t i = 0; i < MESH_NODES_MAX; i++) {
			const struct mesh_peer *p = &m->peers[i];

			if (p->id[0])
				add_node(&b, p, "node", mesh_peer_state(p, now), true,
					 p->drift || p->digest_mismatch || (m->current.version && p->rejected >= m->current.version));
		}
		for (size_t i = 0; i < MESH_CANDIDATES_MAX; i++) {
			const struct mesh_peer *p = &m->candidates[i];
			bool known = false;

			if (!p->id[0])
				continue;
			for (size_t j = 0; j < MESH_NODES_MAX; j++) {
				if (!strcmp(m->peers[j].id, p->id)) {
					known = true;
					break;
				}
			}
			if (!known)
				add_node(&b, p, "candidate", "candidate", false, false);
		}
	}
	blobmsg_close_array(&b, arr);
	return reply(u, req, &b);
}

static int unavailable(RPC_ARGS)
{
	struct blob_buf b = { 0 };

	(void)obj;
	(void)msg;
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "available", false);
	if (!strcmp(method, "client_list")) {
		void *arr = blobmsg_open_array(&b, "clients");
		blobmsg_close_array(&b, arr);
	}
	return reply(u, req, &b);
}

static int roam_config(RPC_ARGS)
{
	struct mesh_core *m = CORE(obj)->core;
	const struct mesh_profile *p = &m->current;
	struct blob_buf b = { 0 };

	UNUSED_REQUEST;
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "available", p->version != 0);
	blobmsg_add_string(&b, "ssid", p->ssid);
	blobmsg_add_u8(&b, "k_neighbor", (p->flags & MESH_K_NEIGHBOR) != 0);
	blobmsg_add_u8(&b, "k_beacon", (p->flags & MESH_K_BEACON) != 0);
	blobmsg_add_u8(&b, "v_bss_transition", (p->flags & MESH_V_BTM) != 0);
	blobmsg_add_u8(&b, "ft", (p->flags & MESH_FT) != 0);
	blobmsg_add_u32(&b, "version", p->version);
	return reply(u, req, &b);
}

static int config_status(RPC_ARGS)
{
	struct mesh_core *m = CORE(obj)->core;
	struct blob_buf b = { 0 };

	UNUSED_REQUEST;
	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "version", m->current.version);
	blobmsg_add_u32(&b, "target_version", m->target_version);
	blobmsg_add_string(&b, "state", mesh_phase_name(m->phase));
	blobmsg_add_string(&b, "error", m->txn_error[0] ? m->txn_error : m->profile_error);
	blobmsg_add_string(&b, "rollout", mesh_rollout_state(m));
	blobmsg_add_u32(&b, "approved_nodes", mesh_node_count(m, false));
	blobmsg_add_u32(&b, "converged_nodes", mesh_node_count(m, true));
	blobmsg_add_u8(&b, "drift", m->drift);
	return reply(u, req, &b);
}

static void check(struct blob_buf *b, const char *name, const char *status, const char *detail)
{
	void *t = blobmsg_open_table(b, NULL);

	blobmsg_add_string(b, "name", name);
	blobmsg_add_string(b, "status", status);
	blobmsg_add_string(b, "detail", detail);
	blobmsg_close_table(b, t);
}

static int diagnose(RPC_ARGS)
{
	struct mesh_core *m = CORE(obj)->core;
	struct blob_buf b = { 0 };
	char error[MESH_ERROR_LEN] = { 0 };
	struct mesh_wifi targets = m->settings.wifi;
	void *arr;

	UNUSED_REQUEST;
	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "checks");
	check(&b, "Service", "PASS", "Local ubus service is responsive");
	check(&b, "Management and identity", !m->settings.valid || m->storage_blocked ? "FAIL" :
	      m->transport ? "PASS" : "WARN", m->error[0] ? m->error : m->transport ?
	      "Transport initialized; check heartbeat separately" : "Mesh service disabled or waiting to start");
	check(&b, "Backhaul", "WARN", "Data backhaul is configured outside this plugin; no path measurement");
	if (strcmp(m->settings.role, "candidate")) {
		int rc = mesh_wifi_precheck(&targets, error, sizeof(error));

		check(&b, "Wireless", rc ? "FAIL" : "PASS", rc ? (error[0] ? error : "Wireless preflight failed") :
		      "Managed AP settings pass static preflight");
	} else {
		check(&b, "Wireless", "WARN", "This device is not configured as a gateway or node");
	}
	check(&b, "Configuration", m->profile_error[0] || m->storage_blocked || m->drift ? "FAIL" :
	      m->current.version ? "PASS" : "WARN", m->profile_error[0] ? m->profile_error : m->txn_error[0] ?
	      m->txn_error : m->drift ? "The live UCI settings differ from the signed profile" :
	      m->current.version ? "Local signed profile recorded" : "No signed profile has been applied");
	blobmsg_close_array(&b, arr);
	return reply(u, req, &b);
}

static int event_log(RPC_ARGS)
{
	struct mesh_core *m = CORE(obj)->core;
	struct blob_buf b = { 0 };
	void *arr;

	UNUSED_REQUEST;
	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "events");
	for (size_t i = 0; i < m->event_count; i++) {
		const struct mesh_event *e = &m->events[(m->event_next + MESH_EVENT_MAX - 1 - i) % MESH_EVENT_MAX];
		void *t = blobmsg_open_table(&b, NULL);

		blobmsg_add_string(&b, "type", e->type);
		blobmsg_add_u64(&b, "time", e->time);
		blobmsg_add_string(&b, "detail", e->detail);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, arr);
	return reply(u, req, &b);
}

static const struct blobmsg_policy node_policy[] = { { "node_id", BLOBMSG_TYPE_STRING } };
static const struct blobmsg_policy apply_policy[] = {
	{ "ssid", BLOBMSG_TYPE_STRING }, { "key", BLOBMSG_TYPE_STRING },
	{ "k_neighbor", BLOBMSG_TYPE_BOOL }, { "k_beacon", BLOBMSG_TYPE_BOOL },
	{ "v_bss_transition", BLOBMSG_TYPE_BOOL }, { "ft", BLOBMSG_TYPE_BOOL },
};

static const char *strict_string(struct blob_attr *a, size_t max)
{
	size_t len = a ? blobmsg_data_len(a) : 0;
	const char *s = a ? blobmsg_get_string(a) : NULL;

	if (!len || len > max + 1 || s[len - 1] || memchr(s, '\0', len - 1))
		return NULL;
	return s;
}

static int change_node(RPC_ARGS)
{
	struct blob_attr *v[ARRAY_SIZE(node_policy)];
	const char *id;

	(void)u; (void)req;
	if (!msg || blobmsg_parse(node_policy, ARRAY_SIZE(v), v, blob_data(msg), blob_len(msg)) ||
	    !(id = strict_string(v[0], MESH_ID_LEN)))
		return UBUS_STATUS_INVALID_ARGUMENT;
	return mesh_allow_change(CORE(obj)->core, id, !strcmp(method, "enroll"));
}

static int config_apply(RPC_ARGS)
{
	struct blob_attr *v[ARRAY_SIZE(apply_policy)];
	struct mesh_profile p = { 0 };
	const char *ssid, *key;
	int rc;
	struct blob_buf b = { 0 };

	(void)method;
	if (!msg || blobmsg_parse(apply_policy, ARRAY_SIZE(v), v, blob_data(msg), blob_len(msg)) ||
	    !(ssid = strict_string(v[0], sizeof(p.ssid) - 1)) || !(key = strict_string(v[1], sizeof(p.key) - 1)))
		return UBUS_STATUS_INVALID_ARGUMENT;
	for (size_t i = 2; i < ARRAY_SIZE(v); i++) {
		if (!v[i] || blobmsg_get_u8(v[i]) > 1)
			return UBUS_STATUS_INVALID_ARGUMENT;
		if (blobmsg_get_bool(v[i]))
			p.flags |= 1u << (i - 2);
	}
	strcpy(p.ssid, ssid);
	strcpy(p.key, key);
	rc = mesh_apply(CORE(obj)->core, &p);
	mbedtls_platform_zeroize(p.key, sizeof(p.key));
	if (rc)
		return rc;
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "accepted", true);
	blobmsg_add_u32(&b, "target_version", p.version);
	return reply(u, req, &b);
}

static int config_rollback(RPC_ARGS)
{
	UNUSED_REQUEST;
	(void)u; (void)req;
	return mesh_cancel(CORE(obj)->core);
}

static int unsupported(RPC_ARGS)
{
	UNUSED_REQUEST;
	(void)u; (void)obj; (void)req;
	return UBUS_STATUS_NOT_SUPPORTED;
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("status", status),
	UBUS_METHOD_NOARG("topology", topology),
	UBUS_METHOD_NOARG("link_quality", unavailable),
	UBUS_METHOD_NOARG("client_list", unavailable),
	UBUS_METHOD_NOARG("roam_config", roam_config),
	UBUS_METHOD_NOARG("config_status", config_status),
	UBUS_METHOD_NOARG("diagnose", diagnose),
	UBUS_METHOD_NOARG("upgrade_status", unavailable),
	UBUS_METHOD_NOARG("event_log", event_log),
	UBUS_METHOD("enroll", change_node, node_policy),
	UBUS_METHOD("remove_node", change_node, node_policy),
	UBUS_METHOD_NOARG("set_role", unsupported),
	UBUS_METHOD_NOARG("set_roam", unsupported),
	UBUS_METHOD_NOARG("set_backhaul", unsupported),
	UBUS_METHOD_NOARG("set_guest", unsupported),
	UBUS_METHOD_NOARG("set_qos", unsupported),
	UBUS_METHOD_NOARG("set_parental", unsupported),
	UBUS_METHOD("config_apply", config_apply, apply_policy),
	UBUS_METHOD_NOARG("config_rollback", config_rollback),
	UBUS_METHOD_NOARG("upgrade_node", unsupported),
	UBUS_METHOD_NOARG("optimize_topology", unsupported),
};

int mesh_rpc_start(struct mesh_core *m)
{
	static struct ubus_object_type type = UBUS_OBJECT_TYPE("meshd", methods);
	static struct mesh_rpc rpc = {
		.obj = { .name = "meshd", .type = &type, .methods = methods, .n_methods = ARRAY_SIZE(methods) },
	};

	rpc.core = m;
	return ubus_add_object(m->ubus, &rpc.obj);
}
