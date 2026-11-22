/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core.h"

#include <errno.h>
#include <fcntl.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SUSPECT_MS 9000u
#define OFFLINE_MS 15000u
#define VERIFY_MS 30000u
#define VERIFY_SETTLE_MS 5000u
#define CANDIDATE_MS 20000u
#define ROLLOUT_GAP_MS 5000u
#define ROLLOUT_WAIT_MS 45000u
#define JOURNAL_LEN (MESH_ID_LEN + 4)

int mesh_host_start(struct mesh_core *m, bool verify);

static void text(char *dst, size_t size, const char *s)
{
	snprintf(dst, size, "%s", s);
}

static uint32_t get32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

bool mesh_is_gateway(const struct mesh_core *m)
{
	return !strcmp(m->settings.role, "gateway");
}

static bool is_node(const struct mesh_core *m)
{
	return !strcmp(m->settings.role, "node");
}

bool mesh_is_busy(const struct mesh_core *m)
{
	return m->phase == PH_APPLYING || m->phase == PH_VERIFYING || m->phase == PH_ROLLING_BACK;
}

const char *mesh_phase_name(enum mesh_phase phase)
{
	static const char *const names[] = { "idle", "applying", "verifying", "committed", "rolling_back", "rolled_back", "failed" };

	return names[phase];
}

const char *mesh_peer_state(const struct mesh_peer *p, uint64_t now)
{
	if (!p->seen || now - p->seen >= OFFLINE_MS)
		return "offline";
	return now - p->seen >= SUSPECT_MS ? "suspect" : "online";
}

void mesh_event(struct mesh_core *m, const char *type, const char *detail)
{
	struct mesh_event *e = &m->events[m->event_next];
	time_t now = time(NULL);

	e->type = type;
	e->time = now < 0 ? 0 : (uint64_t)now;
	text(e->detail, sizeof(e->detail), detail);
	m->event_next = (m->event_next + 1) % MESH_EVENT_MAX;
	if (m->event_count < MESH_EVENT_MAX)
		m->event_count++;
}

static struct mesh_peer *peer_find(struct mesh_peer *list, size_t n, const char *id)
{
	for (size_t i = 0; i < n; i++) {
		if (!strcmp(list[i].id, id))
			return list + i;
	}
	return NULL;
}

static bool converged(const struct mesh_core *m, const struct mesh_peer *p)
{
	return p->id[0] && p->seen && mesh_now_ms() - p->seen < OFFLINE_MS &&
	       !p->drift && !p->digest_mismatch && p->version == m->current.version;
}

size_t mesh_node_count(const struct mesh_core *m, bool converged_only)
{
	size_t n = 0;

	for (size_t i = 0; i < MESH_NODES_MAX; i++) {
		if (m->peers[i].id[0] && (!converged_only || converged(m, &m->peers[i])))
			n++;
	}
	return n;
}

const char *mesh_rollout_state(const struct mesh_core *m)
{
	if (!mesh_is_gateway(m) || !m->current.version)
		return "idle";
	if (m->rollout_paused || m->drift || m->storage_blocked || m->profile_error[0])
		return "paused";
	return mesh_node_count(m, true) == mesh_node_count(m, false) ? "complete" : "waiting";
}

static int load_peers(struct mesh_core *m)
{
	char raw[MESH_NODES_MAX * 65];
	size_t n;
	int rc = mesh_store_read(m->dir, MESH_ALLOW_FILE, raw, sizeof(raw), &n);

	if (rc == 1)
		return 0;
	if (rc || n % 65)
		return -1;
	for (size_t i = 0; i < n / 65; i++) {
		char *s = raw + i * 65;

		if (s[64] != '\n')
			goto fail;
		s[64] = '\0';
		if (!mesh_id_valid(s) || peer_find(m->peers, i, s))
			goto fail;
		strcpy(m->peers[i].id, s);
	}
	return 0;
fail:
	memset(m->peers, 0, sizeof(m->peers));
	return -1;
}

int mesh_allow_change(struct mesh_core *m, const char *id, bool allow)
{
	struct mesh_peer *p;
	char raw[MESH_NODES_MAX * 65];
	size_t n = 0;
	int rc;

	if (!m->settings.enabled || !mesh_is_gateway(m) || !m->transport || m->storage_blocked)
		return UBUS_STATUS_PERMISSION_DENIED;
	if (!mesh_id_valid(id) || !strcmp(id, m->id))
		return UBUS_STATUS_INVALID_ARGUMENT;
	p = peer_find(m->peers, MESH_NODES_MAX, id);
	if (allow) {
		if (p)
			return UBUS_STATUS_OK;
		p = peer_find(m->peers, MESH_NODES_MAX, "");
		if (!p)
			return UBUS_STATUS_NO_MEMORY;
	} else if (!p) {
		return UBUS_STATUS_NOT_FOUND;
	}
	for (size_t i = 0; i < MESH_NODES_MAX; i++) {
		if (!m->peers[i].id[0] || &m->peers[i] == p)
			continue;
		memcpy(raw + n, m->peers[i].id, 64);
		raw[n + 64] = '\n'; n += 65;
	}
	if (allow) {
		memcpy(raw + n, id, 64);
		raw[n + 64] = '\n'; n += 65;
	}
	rc = mesh_store_write(m->dir, MESH_ALLOW_FILE, raw, n);
	if (rc) {
		if (rc == -2) {
			m->storage_blocked = true;
			text(m->error, sizeof(m->error), "Cannot confirm durability of node authorizations; restart and review");
		}
		return UBUS_STATUS_SYSTEM_ERROR;
	}
	if (allow) {
		strcpy(p->id, id);
	} else {
		memset(p, 0, sizeof(*p));
		if (!strcmp(m->active_peer, id)) {
			m->active_peer[0] = 0;
			m->rollout_after = mesh_now_ms() + ROLLOUT_GAP_MS;
		}
	}
	mesh_event(m, allow ? "node_approved" : "node_removed", id);
	return UBUS_STATUS_OK;
}

static size_t record_create(unsigned char *out, const void *der, size_t der_len,
			    const unsigned char *signed_payload, size_t payload_len)
{
	if (!der || !der_len || der_len > MESH_CERT_MAX || !payload_len || payload_len > MESH_SIGNED_MAX)
		return 0;
	out[0] = der_len >> 8;
	out[1] = der_len;
	memcpy(out + 2, der, der_len);
	memcpy(out + 2 + der_len, signed_payload, payload_len);
	return 2 + der_len + payload_len;
}

static const unsigned char *record_payload(const unsigned char *record, size_t len, size_t *payload_len)
{
	size_t der_len;

	if (len < 3)
		return NULL;
	der_len = (size_t)record[0] << 8 | record[1];
	if (!der_len || der_len > MESH_CERT_MAX || len <= 2 + der_len)
		return NULL;
	*payload_len = len - 2 - der_len;
	return record + 2 + der_len;
}

static bool record_verify(struct mesh_core *m, const unsigned char *record, size_t len, struct mesh_profile *p)
{
	struct mesh_signature_view v;
	char id[65];
	size_t payload_len;
	const unsigned char *b = record_payload(record, len, &payload_len);

	if (!b)
		return false;
	mesh_hash_hex(record + 2, (size_t)(b - (record + 2)), id);
	if (strcmp(id, mesh_is_gateway(m) ? m->id : m->settings.gateway_id))
		return false;
	return mesh_signed_decode(b, payload_len, m->scope, p, &v) &&
	       !mesh_transport_verify(record + 2, (size_t)(b - (record + 2)), v.bytes, v.payload_len,
				      v.signature, v.signature_len);
}

static bool record_hash(const unsigned char *record, size_t len, unsigned char hash[MESH_SCOPE_LEN])
{
	size_t n;
	const unsigned char *payload = record_payload(record, len, &n);

	return payload && !mbedtls_sha256(payload, n, hash, 0);
}

static void refresh_drift(struct mesh_core *m)
{
	bool drift = m->current.version && mesh_wifi_matches(&m->settings.wifi, &m->current) != 0;

	if (drift != m->drift) {
		m->drift = drift;
		mesh_event(m, drift ? "config_drift" : "config_drift_cleared", "Local managed wireless configuration");
	}
}

static void load_profile(struct mesh_core *m)
{
	size_t n;
	struct mesh_profile p = { 0 };
	int rc = mesh_store_read(m->dir, MESH_PROFILE_FILE, m->record, sizeof(m->record), &n);

	if (rc == 1)
		return;
	if (rc || !record_verify(m, m->record, n, &p) || !record_hash(m->record, n, m->profile_hash)) {
		text(m->profile_error, sizeof(m->profile_error), "Stored profile is unreadable, invalid or bound to a different identity/network");
		mesh_event(m, "profile_rejected", m->profile_error);
	} else {
		m->record_len = n;
		m->current = p;
		refresh_drift(m);
		if (m->phase == PH_IDLE)
			m->phase = PH_COMMITTED;
	}
	mbedtls_platform_zeroize(&p, sizeof(p));
}

static void drop_pending(struct mesh_core *m)
{
	mbedtls_platform_zeroize(&m->pending, sizeof(m->pending));
	mbedtls_platform_zeroize(m->staged, sizeof(m->staged));
	m->staged_len = 0;
	m->cancel = false;
}

static bool cleanup_txn(struct mesh_core *m)
{
	/* 顺序不可颠倒：先保证日志不再可见，再移走恢复所需的文件。 */
	if (mesh_store_remove(m->dir, MESH_JOURNAL_FILE)) {
		m->storage_blocked = true;
		text(m->txn_error, sizeof(m->txn_error), "Could not clear transaction marker; inspect storage and restart");
		return false;
	}
	if (mesh_wifi_clear())
		mesh_event(m, "cleanup_warning", "Unable to remove stale wireless staging files");
	return true;
}

static bool remember_rejection(struct mesh_core *m, uint32_t version)
{
	unsigned char bytes[4];

	if (!is_node(m) || version <= m->rejected)
		return true;
	put32(bytes, version);
	if (mesh_store_write(m->dir, MESH_FAILED_FILE, bytes, sizeof(bytes))) {
		m->storage_blocked = true;
		return false;
	}
	m->rejected = version;
	return true;
}

static void load_rejection(struct mesh_core *m)
{
	unsigned char bytes[4];
	size_t len;
	int rc;

	if (!is_node(m))
		return;
	rc = mesh_store_read(m->dir, MESH_FAILED_FILE, bytes, sizeof(bytes), &len);
	if (rc == 1)
		return;
	if (rc || len != sizeof(bytes)) {
		m->storage_blocked = true;
		text(m->error, sizeof(m->error), "Cannot read the last rejected configuration version");
		return;
	}
	m->rejected = get32(bytes);
}

static void begin_rollback(struct mesh_core *m, const char *why)
{
	text(m->txn_error, sizeof(m->txn_error), why);
	m->phase = PH_ROLLING_BACK;
	m->rollback_disk_done = false;
	m->deadline = mesh_now_ms() + VERIFY_MS;
	mesh_event(m, "config_rollback", why);
	uloop_timeout_set(&m->txn_tick, 1);
}

static void recover(struct mesh_core *m)
{
	unsigned char journal[JOURNAL_LEN];
	unsigned char tmp[MESH_RECORD_MAX];
	char digest[65], recorded[65];
	size_t size, n;
	int rc = mesh_store_read(m->dir, MESH_JOURNAL_FILE, journal, sizeof(journal), &size);

	if (rc == 1) {
		if (mesh_wifi_clear())
			mesh_event(m, "cleanup_warning", "Unable to remove stale wireless staging files");
		return;
	}
	if (rc || size != sizeof(journal))
		goto fail;
	memcpy(recorded, journal, 64);
	recorded[64] = 0;
	m->target_version = get32(journal + 64);
	if (!mesh_id_valid(recorded) || !m->target_version)
		goto fail;
	rc = mesh_store_read(m->dir, MESH_PROFILE_FILE, tmp, sizeof(tmp), &n);
	if (rc < 0)
		goto fail;
	if (!rc) {
		mesh_hash_hex(tmp, n, digest);
		mbedtls_platform_zeroize(tmp, sizeof(tmp));
		if (!strcmp(digest, recorded)) {
			if (!cleanup_txn(m))
				m->phase = PH_FAILED;
			return;
		}
	}
	m->rollback_marker = true;
	begin_rollback(m, "An interrupted wireless update will be restored");
	return;
fail:
	m->storage_blocked = true;
	m->phase = PH_FAILED;
	text(m->txn_error, sizeof(m->txn_error), "Transaction recovery metadata is unreadable; backup preserved");
	mesh_event(m, "recovery_needed", m->txn_error);
}

static void txn_failed(struct mesh_core *m, const char *detail, bool block)
{
	text(m->txn_error, sizeof(m->txn_error), detail);
	m->phase = PH_FAILED;
	m->storage_blocked |= block;
	mesh_event(m, "config_failed", detail);
	drop_pending(m);
}

static void txn_unpublished(struct mesh_core *m, const char *detail)
{
	/* 无无线写入，拒绝此版本但不覆盖管理员并行提交。 */
	if (!remember_rejection(m, m->target_version) || !cleanup_txn(m)) {
		txn_failed(m, "Cannot persist transaction abort; inspect storage and restart", true);
		return;
	}
	txn_failed(m, detail, false);
}

static void run_apply(struct mesh_core *m)
{
	char error[MESH_ERROR_LEN] = { 0 }, digest[65];
	unsigned char journal[JOURNAL_LEN];
	int rc;

	if (m->cancel) {
		m->phase = PH_ROLLED_BACK;
		text(m->txn_error, sizeof(m->txn_error), "Cancelled before wireless settings were changed");
		drop_pending(m);
		return;
	}
	if (mesh_wifi_stage(&m->settings.wifi, &m->pending, error, sizeof(error))) {
		txn_unpublished(m, error[0] ? error : "Wireless preflight or staging failed");
		return;
	}
	mesh_hash_hex(m->staged, m->staged_len, digest);
	memcpy(journal, digest, 64);
	put32(journal + 64, m->target_version);
	rc = mesh_store_write(m->dir, MESH_JOURNAL_FILE, journal, sizeof(journal));
	if (rc) {
		txn_unpublished(m, "Could not persist transaction marker before changing wireless");
		return;
	}
	m->rollback_marker = true;
	rc = mesh_wifi_publish(error, sizeof(error));
	if (rc == -ECANCELED) {
		txn_unpublished(m, error[0] ? error : "Wireless settings changed during staging");
		return;
	}
	if (rc || mesh_host_start(m, false)) {
		begin_rollback(m, error[0] ? error : "Could not publish or reload wireless settings");
		return;
	}
	m->deadline = mesh_now_ms() + VERIFY_MS;
	uloop_timeout_set(&m->txn_tick, 300);
}

static void commit(struct mesh_core *m)
{
	unsigned char digest[MESH_SCOPE_LEN];
	int rc;

	if (!record_hash(m->staged, m->staged_len, digest)) {
		begin_rollback(m, "Cannot hash the staged signed profile");
		return;
	}
	rc = mesh_store_write(m->dir, MESH_PROFILE_FILE, m->staged, m->staged_len);
	if (rc == -1) {
		begin_rollback(m, "Cannot store the verified profile");
		return;
	}
	if (rc == -2) {
		/* rename 已发生，不能与文件系统可能已持久化的提交做相反决定。 */
		txn_failed(m, "Storage did not confirm profile commit; restart with transaction files intact", true);
		return;
	}
	if (!cleanup_txn(m)) {
		txn_failed(m, "Profile was stored but transaction cleanup requires a restart", true);
		return;
	}
	m->current = m->pending;
	memcpy(m->record, m->staged, m->staged_len);
	m->record_len = m->staged_len;
	memcpy(m->profile_hash, digest, sizeof(digest));
	m->drift = false;
	m->phase = PH_COMMITTED;
	m->txn_error[0] = 0;
	m->rollout_paused = false;
	m->active_peer[0] = '\0';
	m->rollout_after = mesh_now_ms() + ROLLOUT_GAP_MS;
	m->rollback_marker = false;
	mesh_event(m, "config_committed", "Local wireless health check passed");
	drop_pending(m);
}

static void run_rollback(struct mesh_core *m)
{
	char error[MESH_ERROR_LEN] = { 0 };

	if (!m->rollback_disk_done) {
		if (!m->rollback_marker || mesh_wifi_restore(error, sizeof(error))) {
			txn_failed(m, error[0] ? error : "Wireless backup could not be restored", true);
			return;
		}
		m->rollback_disk_done = true;
	}
	if (mesh_now_ms() >= m->deadline) {
		txn_failed(m, "Backup restored on disk, but wireless reload failed; fix and restart", true);
		return;
	}
	if (mesh_host_start(m, false))
		uloop_timeout_set(&m->txn_tick, 850 + mesh_jitter(250));
	else
		uloop_timeout_set(&m->txn_tick, 400);
}

static void worker_done(struct uloop_process *p, int status)
{
	struct mesh_core *m = container_of(p, struct mesh_core, worker);
	bool good = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	if (m->phase == PH_ROLLING_BACK) {
		if (good && m->rollback_disk_done) {
			if (!remember_rejection(m, m->target_version) || !cleanup_txn(m)) {
				txn_failed(m, "Wireless restored but recovery metadata requires a restart", true);
				return;
			}
			m->phase = PH_ROLLED_BACK;
			m->rollback_marker = false;
			mesh_event(m, "config_rolled_back", "Previous local wireless settings restored");
			drop_pending(m);
			refresh_drift(m);
			return;
		}
	} else if (m->phase == PH_APPLYING) {
		if (!good || m->cancel) {
			begin_rollback(m, m->cancel ? "Cancelled by administrator" : "Network reload failed or timed out");
			return;
		}
		m->phase = PH_VERIFYING;
		m->verify_since = mesh_now_ms();
	} else if (m->phase == PH_VERIFYING && good && !m->cancel && mesh_now_ms() <= m->deadline) {
		if ((!is_node(m) || m->last_gateway >= m->verify_since) &&
		    mesh_wifi_matches(&m->settings.wifi, &m->pending) == 0) {
			commit(m);
			return;
		}
	}
	uloop_timeout_set(&m->txn_tick, 850 + mesh_jitter(250));
}

static void txn_tick(struct uloop_timeout *t)
{
	struct mesh_core *m = container_of(t, struct mesh_core, txn_tick);
	uint64_t now = mesh_now_ms();

	if (!mesh_is_busy(m))
		return;
	if (m->worker.pending) {
		if (now >= m->worker_deadline)
			kill(m->worker.pid, SIGKILL);
		uloop_timeout_set(t, 300 + mesh_jitter(100));
		return;
	}
	switch (m->phase) {
	case PH_APPLYING:
		run_apply(m);
		break;
	case PH_VERIFYING:
		if (m->cancel || now >= m->deadline) {
			begin_rollback(m, m->cancel ? "Cancelled by administrator" : "AP or authenticated management health check timed out");
		} else if (now - m->verify_since < VERIFY_SETTLE_MS) {
			uloop_timeout_set(t, VERIFY_SETTLE_MS - (unsigned int)(now - m->verify_since) + mesh_jitter(200));
		} else {
			int rc = mesh_host_start(m, true);
			uloop_timeout_set(t, rc ? 850 + mesh_jitter(250) : 400);
		}
		break;
	case PH_ROLLING_BACK:
		run_rollback(m);
		break;
	default:
		break;
	}
}

static void queue_apply(struct mesh_core *m, const struct mesh_profile *p)
{
	m->pending = *p;
	m->target_version = p->version;
	m->txn_error[0] = 0;
	m->phase = PH_APPLYING;
	m->rollback_marker = false;
	m->cancel = false;
	mesh_event(m, "config_applying", "Verifying a new wireless profile locally");
	uloop_timeout_set(&m->txn_tick, 1);
}

int mesh_apply(struct mesh_core *m, struct mesh_profile *p)
{
	unsigned char signed_payload[MESH_SIGNED_MAX], sig[MESH_SIGNATURE_MAX];
	size_t n, der_len, sig_len = sizeof(sig);
	const void *der;
	int rc = UBUS_STATUS_SYSTEM_ERROR;

	if (!m->settings.enabled || !mesh_is_gateway(m) || !m->transport)
		return UBUS_STATUS_PERMISSION_DENIED;
	if (m->storage_blocked || m->profile_error[0])
		return UBUS_STATUS_SYSTEM_ERROR;
	if (mesh_is_busy(m))
		return UBUS_STATUS_NOT_SUPPORTED;
	if (m->current.version == UINT32_MAX)
		return UBUS_STATUS_NOT_SUPPORTED;
	p->version = m->current.version + 1;
	if (!mesh_profile_valid(p))
		return UBUS_STATUS_INVALID_ARGUMENT;
	n = mesh_payload_encode(p, m->scope, signed_payload, sizeof(signed_payload));
	if (!n || mesh_transport_sign(m->transport, signed_payload, n, sig, &sig_len) ||
	    mesh_transport_certificate(m->transport, &der, &der_len))
		goto out;
	n = mesh_signed_append(signed_payload, n, sizeof(signed_payload), sig, sig_len);
	m->staged_len = record_create(m->staged, der, der_len, signed_payload, n);
	if (!m->staged_len)
		goto out;
	queue_apply(m, p);
	rc = UBUS_STATUS_OK;
out:
	mbedtls_platform_zeroize(signed_payload, sizeof(signed_payload));
	return rc;
}

int mesh_cancel(struct mesh_core *m)
{
	if (!mesh_is_gateway(m))
		return UBUS_STATUS_PERMISSION_DENIED;
	if (m->phase != PH_APPLYING && m->phase != PH_VERIFYING)
		return UBUS_STATUS_NOT_SUPPORTED;
	m->cancel = true;
	uloop_timeout_set(&m->txn_tick, 1);
	return UBUS_STATUS_OK;
}

static void discovery(void *arg, const char *id, const char *ip, const char *model)
{
	struct mesh_core *m = arg;
	struct mesh_peer *p;
	uint64_t now = mesh_now_ms();

	if (!mesh_id_valid(id) || !strcmp(m->id, id) || !mesh_model_valid(model) ||
	    peer_find(m->peers, MESH_NODES_MAX, id))
		return;
	p = peer_find(m->candidates, MESH_CANDIDATES_MAX, id);
	if (!p) {
		for (size_t i = 0; i < MESH_CANDIDATES_MAX; i++) {
			if (!m->candidates[i].seen || now - m->candidates[i].seen >= CANDIDATE_MS) {
				p = &m->candidates[i];
				break;
			}
		}
	}
	if (!p)
		return;
	strcpy(p->id, id);
	text(p->ip, sizeof(p->ip), ip);
	text(p->model, sizeof(p->model), model);
	p->seen = now;
}

static bool allowed(void *arg, const char *id)
{
	struct mesh_core *m = arg;

	return !m->storage_blocked && strcmp(id, m->id) && peer_find(m->peers, MESH_NODES_MAX, id);
}

static size_t node_request(void *arg, unsigned char *out, size_t capacity)
{
	struct mesh_core *m = arg;
	struct mesh_heartbeat h = { .version = m->current.version, .rejected_version = m->rejected,
		.drift = m->drift || m->storage_blocked || m->profile_error[0] };

	memcpy(h.profile_hash, m->profile_hash, sizeof(h.profile_hash));
	strcpy(h.model, m->settings.model);
	return mesh_heartbeat_encode(&h, m->scope, out, capacity);
}

static void pause_rollout(struct mesh_core *m, const char *reason)
{
	if (!m->rollout_paused) {
		m->rollout_paused = true;
		mesh_event(m, "rollout_paused", reason);
	}
}

static size_t gateway_request(void *arg, const char *id, const char *ip, const unsigned char *request,
			      size_t request_len, unsigned char *out, size_t capacity)
{
	struct mesh_core *m = arg;
	struct mesh_peer *p = peer_find(m->peers, MESH_NODES_MAX, id);
	struct mesh_heartbeat h;
	uint64_t now = mesh_now_ms();
	const unsigned char *payload;
	size_t n;
	uint32_t cur = m->current.version;

	if (!p || capacity < sizeof(mesh_ack) || !mesh_heartbeat_decode(request, request_len, m->scope, &h))
		return 0;
	if (!p->seen || now - p->seen >= OFFLINE_MS)
		mesh_event(m, "node_online", id);
	p->seen = now;
	p->observed_state = 1;
	p->version = h.version;
	p->rejected = h.rejected_version;
	p->drift = h.drift;
	p->digest_mismatch = cur && h.version == cur &&
			     memcmp(h.profile_hash, m->profile_hash, sizeof(h.profile_hash));
	text(p->ip, sizeof(p->ip), ip);
	strcpy(p->model, h.model);
	if (cur && (h.rejected_version >= cur || h.version > cur || p->digest_mismatch || (h.version == cur && h.drift)))
		pause_rollout(m, "An authorized node reported rejection, a conflicting version or local drift");
	if (m->active_peer[0] && !strcmp(m->active_peer, id) && converged(m, p)) {
		m->active_peer[0] = 0;
		m->rollout_after = now + ROLLOUT_GAP_MS;
		mesh_event(m, "node_config_committed", id);
	}
	if (!cur || h.version >= cur || m->rollout_paused || mesh_is_busy(m) || m->drift || m->profile_error[0] ||
	    m->storage_blocked || now < m->rollout_after || (m->active_peer[0] && strcmp(m->active_peer, id)))
		goto ack;
	payload = record_payload(m->record, m->record_len, &n);
	if (!payload || n > capacity)
		goto ack;
	if (!m->active_peer[0]) {
		strcpy(m->active_peer, id);
		m->rollout_deadline = now + ROLLOUT_WAIT_MS;
		mesh_event(m, "node_config_offered", id);
	}
	memcpy(out, payload, n);
	return n;
ack:
	memcpy(out, mesh_ack, sizeof(mesh_ack));
	return sizeof(mesh_ack);
}

static void node_response(void *arg, const unsigned char *response, size_t len, const void *der, size_t der_len)
{
	struct mesh_core *m = arg;
	struct mesh_profile p = { 0 };
	struct mesh_signature_view sig;

	if (len == sizeof(mesh_ack) && !memcmp(response, mesh_ack, len)) {
		m->last_gateway = mesh_now_ms();
		return;
	}
	if (!mesh_signed_decode(response, len, m->scope, &p, &sig) ||
	    mesh_transport_verify(der, der_len, sig.bytes, sig.payload_len, sig.signature, sig.signature_len))
		goto out;
	m->last_gateway = mesh_now_ms();
	if (m->storage_blocked || m->profile_error[0] || mesh_is_busy(m) ||
	    p.version <= m->current.version || p.version <= m->rejected)
		goto out;
	m->staged_len = record_create(m->staged, der, der_len, response, len);
	if (m->staged_len)
		queue_apply(m, &p);
out:
	mbedtls_platform_zeroize(&p, sizeof(p));
}

static void start_transport(struct mesh_core *m)
{
	struct mesh_settings *s = &m->settings;
	const struct mesh_transport_options opt = {
		.bind_ip = s->bind_ip, .broadcast_ip = s->broadcast_ip, .gateway_ip = s->gateway_ip,
		.gateway_id = s->gateway_id, .network = s->network, .model = s->model,
		.cert_file = MESH_DATA "/identity/node.crt", .key_file = MESH_DATA "/identity/node.key",
		.ca_file = MESH_DATA "/identity/ca.crt", .gateway = mesh_is_gateway(m),
		.candidate = !strcmp(s->role, "candidate"),
	};
	static const struct mesh_transport_ops ops = {
		.discovered = discovery, .is_allowed = allowed, .node_request = node_request,
		.gateway_request = gateway_request, .node_response = node_response,
	};

	m->transport = mesh_transport_start(&opt, &ops, m, m->error, sizeof(m->error));
	if (!m->transport)
		return;
	if (mesh_transport_fingerprint(m->transport, m->id)) {
		mesh_transport_stop(m->transport);
		m->transport = NULL;
		text(m->error, sizeof(m->error), "Could not obtain local certificate fingerprint");
		return;
	}
	m->error[0] = '\0';
	if (opt.gateway || is_node(m))
		load_profile(m);
	mesh_event(m, "transport_started", s->role);
}

static void core_tick(struct uloop_timeout *t)
{
	struct mesh_core *m = container_of(t, struct mesh_core, core_tick);
	uint64_t now = mesh_now_ms();

	if (m->settings.enabled && m->settings.valid && !m->transport && !m->storage_blocked && !mesh_is_busy(m))
		start_transport(m);
	if (!mesh_is_busy(m))
		refresh_drift(m);
	for (size_t i = 0; i < MESH_NODES_MAX; i++) {
		struct mesh_peer *p = &m->peers[i];
		unsigned int state;

		if (!p->id[0] || !p->seen)
			continue;
		state = now - p->seen >= OFFLINE_MS ? 3 : now - p->seen >= SUSPECT_MS ? 2 : 1;
		if (state != p->observed_state) {
			p->observed_state = state;
			mesh_event(m, state == 3 ? "node_offline" : state == 2 ? "node_suspect" : "node_online", p->id);
		}
	}
	for (size_t i = 0; i < MESH_CANDIDATES_MAX; i++) {
		struct mesh_peer *p = &m->candidates[i];

		if (p->seen && now - p->seen >= CANDIDATE_MS)
			memset(p, 0, sizeof(*p));
	}
	if (m->active_peer[0] && now >= m->rollout_deadline)
		pause_rollout(m, "Timed out waiting for the selected node to confirm its profile");
	uloop_timeout_set(t, 1350 + mesh_jitter(400));
}

int main(void)
{
	static struct mesh_core m;
	int rc = 1;

	umask(0077);
	signal(SIGPIPE, SIG_IGN);
	if (uloop_init())
		return 1;
	m.dir = mesh_store_open();
	if (m.dir >= 0 && flock(m.dir, LOCK_EX | LOCK_NB)) {
		fprintf(stderr, "meshd: another instance owns the state directory, or cannot lock it\n");
		goto end;
	}
	mesh_load_settings(&m.settings, m.error, sizeof(m.error));
	m.txn_tick.cb = txn_tick;
	m.core_tick.cb = core_tick;
	m.worker.cb = worker_done;
	if (m.dir < 0) {
		m.storage_blocked = true;
		text(m.error, sizeof(m.error), "Missing or insecure /etc/meshd storage directory");
	} else {
		load_rejection(&m);
		recover(&m);
		if (mesh_is_gateway(&m) && load_peers(&m)) {
			m.storage_blocked = true;
			text(m.error, sizeof(m.error), "Invalid authorization list; no nodes trusted");
		}
	}
	if (m.settings.enabled && m.settings.valid &&
	    mbedtls_sha256((const unsigned char *)m.settings.network, strlen(m.settings.network), m.scope, 0)) {
		m.storage_blocked = true;
		text(m.error, sizeof(m.error), "Network scope hash failed");
	}
	m.ubus = ubus_connect(NULL);
	if (!m.ubus || mesh_rpc_start(&m)) {
		fprintf(stderr, "meshd: unable to publish ubus object\n");
		goto end;
	}
	ubus_add_uloop(m.ubus);
	uloop_timeout_set(&m.core_tick, 1 + mesh_jitter(500));
	uloop_run();
	rc = m.ubus->sock.eof || m.ubus->sock.error ? 1 : 0;
end:
	if (m.worker.pending) {
		uloop_process_delete(&m.worker);
		kill(m.worker.pid, SIGKILL);
		waitpid(m.worker.pid, NULL, 0);
	}
	if (m.transport)
		mesh_transport_stop(m.transport);
	if (m.ubus)
		ubus_free(m.ubus);
	if (m.dir >= 0)
		close(m.dir);
	uloop_done();
	mbedtls_platform_zeroize(&m, sizeof(m));
	return rc;
}
