/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

/* 单独的短时子进程隔离 ubus_lookup_id（libubus 没有 lookup timeout）。
 * 主进程统一负责超时和重新尝试，不在控制面主循环等待 netifd/hostapd。 */
enum { HOST_RELOAD = 1, HOST_VERIFY };

struct ap_status {
	const char *expected;
	bool good;
};

static void status_received(struct ubus_request *r, int type, struct blob_attr *msg)
{
	static const struct blobmsg_policy policy[] = {
		{ "status", BLOBMSG_TYPE_STRING }, { "ssid", BLOBMSG_TYPE_STRING },
	};
	struct blob_attr *v[2];
	struct ap_status *s = r->priv;

	(void)type;
	if (!msg || blobmsg_parse(policy, 2, v, blob_data(msg), blob_len(msg)) || !v[0] || !v[1])
		return;
	s->good = !strcmp(blobmsg_get_string(v[0]), "ENABLED") &&
		   !strcmp(blobmsg_get_string(v[1]), s->expected);
}

static int host_call(const struct mesh_core *m, int action)
{
	struct ubus_context *u = ubus_connect(NULL);
	uint32_t id;
	int rc = 1;

	if (!u)
		return 1;
	if (action == HOST_RELOAD) {
		if (!ubus_lookup_id(u, "network", &id) && !ubus_invoke(u, id, "reload", NULL, NULL, NULL, 3000))
			rc = 0;
	} else {
		for (size_t i = 0; i < m->settings.wifi.count; i++) {
			char object[sizeof("hostapd.") + MESH_IFNAME_MAX];
			struct ap_status status = { .expected = m->pending.ssid };

			snprintf(object, sizeof(object), "hostapd.%s", m->settings.wifi.ifnames[i]);
			if (ubus_lookup_id(u, object, &id) ||
			    ubus_invoke(u, id, "get_status", NULL, status_received, &status, 600) || !status.good)
				goto out;
		}
		rc = 0;
	}
out:
	ubus_free(u);
	return rc;
}

int mesh_host_start(struct mesh_core *m, bool verify)
{
	pid_t parent = getpid(), pid;
	int action = verify ? HOST_VERIFY : HOST_RELOAD;

	if (m->worker.pending)
		return -1;
	pid = fork();
	if (pid < 0)
		return -1;
	if (!pid) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
			_exit(1);
		alarm(7);
		/* 此子进程不用父进程的 ubus socket；也不从继承的 uloop epoll 读事件。 */
		if (m->ubus && m->ubus->sock.fd >= 0)
			close(m->ubus->sock.fd);
		close(m->dir);
		_exit(host_call(m, action));
	}
	m->worker.pid = pid;
	m->worker_deadline = mesh_now_ms() + (verify ? 6000 : 4200);
	if (uloop_process_add(&m->worker)) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return -1;
	}
	return 0;
}
