/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "local.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <uci.h>
#include <unistd.h>

static struct uci_section *section(struct uci_package *p, const char *name)
{
	struct uci_element *e;

	uci_foreach_element(&p->sections, e) {
		struct uci_section *s = uci_to_section(e);
		if (!strcmp(e->name, name) && !strcmp(s->type, "mesh"))
			return s;
	}
	return NULL;
}

static const char *option(struct uci_context *u, struct uci_section *s, const char *name)
{
	const char *v = s ? uci_lookup_option_string(u, s, name) : NULL;

	return v ? v : "";
}

static bool copy(char *out, size_t size, const char *value)
{
	if (strlen(value) >= size)
		return false;
	strcpy(out, value);
	return true;
}

static bool ipv4(const char *s)
{
	struct in_addr a;

	return inet_pton(AF_INET, s, &a) == 1;
}

static bool section_name(const char *s)
{
	if (!*s)
		return false;
	for (; *s; s++) {
		if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') ||
		      (*s >= 'A' && *s <= 'Z') || *s == '_'))
			return false;
	}
	return true;
}

void mesh_load_settings(struct mesh_settings *s, char *error, size_t error_len)
{
	struct uci_context *u = uci_alloc_context();
	struct uci_package *p;
	struct uci_section *core, *backhaul, *security, *roam;
	const char *enabled, *role, *model, *protocol;
	struct uci_option *targets;
	const char *why = "Cannot load committed /etc/config/meshd";

	strcpy(s->role, "candidate");
	strcpy(s->model, "OpenWrt");
	strcpy(s->backhaul, "manual");
	if (!u)
		goto fail;
	/* 绝对路径让 libuci 不应用 savedir 或 conf2 的未提交编辑。 */
	if (uci_load(u, "/etc/config/meshd", &p))
		goto fail;
	core = section(p, "core");
	if (!core)
		goto fail;
	backhaul = section(p, "backhaul");
	security = section(p, "security");
	roam = section(p, "roam");
	enabled = option(u, core, "enabled");
	role = option(u, core, "role");
	model = option(u, core, "model");
	protocol = option(u, backhaul, "protocol");
	why = "Invalid enabled, role, model or backhaul setting";
	if ((strcmp(enabled, "0") && strcmp(enabled, "1")) ||
	    (strcmp(role, "gateway") && strcmp(role, "node") && strcmp(role, "candidate")) ||
	    (*model && !mesh_model_valid(model)) ||
	    (*protocol && strcmp(protocol, "auto") && strcmp(protocol, "wired") && strcmp(protocol, "wds")))
		goto fail;
	s->enabled = !strcmp(enabled, "1");
	strcpy(s->role, role);
	if (*model)
		strcpy(s->model, model);
	if (*protocol && strcmp(protocol, "auto"))
		strcpy(s->backhaul, protocol);
	if (!s->enabled)
		goto done;
	why = "mTLS is mandatory; WPS and automatic enrollment are not available";
	if (strcmp(option(u, security, "mtls"), "1") || strcmp(option(u, security, "allow_wps"), "0"))
		goto fail;
	why = "Set a network name and a dedicated IPv4 management address";
	if (!*option(u, core, "net_name") ||
	    !copy(s->network, sizeof(s->network), option(u, core, "net_name")) ||
	    !copy(s->bind_ip, sizeof(s->bind_ip), option(u, core, "bind_ip")) || !ipv4(s->bind_ip) ||
	    !copy(s->broadcast_ip, sizeof(s->broadcast_ip), option(u, core, "broadcast_ip")))
		goto fail;
	if (!strcmp(role, "node")) {
		why = "Set a gateway IPv4 address and the exact SHA-256 certificate fingerprint";
		if (!copy(s->gateway_ip, sizeof(s->gateway_ip), option(u, core, "gateway_ip")) ||
		    !ipv4(s->gateway_ip) || !strcmp(s->gateway_ip, s->bind_ip) ||
		    !mesh_id_valid(option(u, core, "gateway_id")))
			goto fail;
		strcpy(s->gateway_id, option(u, core, "gateway_id"));
	} else if (!ipv4(s->broadcast_ip)) {
		why = "Set the IPv4 broadcast address of the management network";
		goto fail;
	}
	targets = roam ? uci_lookup_option(u, roam, "ap") : NULL;
	why = "Specify named client AP sections as config roam list ap entries";
	if (targets) {
		struct uci_element *e;

		if (targets->type != UCI_TYPE_LIST)
			goto fail;
		uci_foreach_element(&targets->v.list, e) {
			if (s->wifi.count == MESH_AP_MAX || !section_name(e->name) ||
			    !copy(s->wifi.sections[s->wifi.count], MESH_SECTION_MAX, e->name))
				goto fail;
			for (size_t n = 0; n < s->wifi.count; n++) {
				if (!strcmp(s->wifi.sections[n], e->name))
					goto fail;
			}
			s->wifi.count++;
		}
	}

done:
	s->valid = true;
	uci_free_context(u);
	return;
fail:
	snprintf(error, error_len, "%s", why);
	if (u)
		uci_free_context(u);
}

int mesh_store_open(void)
{
	struct stat st;
	int fd = open(MESH_DATA, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || st.st_uid != 0 || (st.st_mode & 0077)) {
		close(fd);
		return -1;
	}
	return fd;
}

int mesh_store_read(int dir, const char *name, void *data, size_t cap, size_t *size)
{
	struct stat st;
	unsigned char *out = data;
	unsigned char extra;
	size_t used = 0;
	ssize_t n;
	int fd = openat(dir, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);

	if (fd < 0)
		return errno == ENOENT ? 1 : -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077) ||
	    st.st_size < 0 || (uint64_t)st.st_size > cap)
		goto fail;
	while (used < cap) {
		n = read(fd, out + used, cap - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			goto fail;
		if (!n)
			break;
		used += (size_t)n;
	}
	do {
		n = read(fd, &extra, 1);
	} while (n < 0 && errno == EINTR);
	if (n != 0)
		goto fail;
	close(fd);
	*size = used;
	return 0;
fail:
	close(fd);
	return -1;
}

int mesh_store_write(int dir, const char *name, const void *data, size_t len)
{
	unsigned int token;
	char tmp[32];
	const unsigned char *bytes = data;
	int fd;

	if (getrandom(&token, sizeof(token), 0) != sizeof(token))
		return -1;
	snprintf(tmp, sizeof(tmp), ".meshd-%08x", token);
	fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	while (len) {
		ssize_t n = write(fd, bytes, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			goto fail;
		bytes += n;
		len -= (size_t)n;
	}
	if (fsync(fd))
		goto fail;
	if (close(fd) || renameat(dir, tmp, dir, name)) {
		unlinkat(dir, tmp, 0);
		return -1;
	}
	return fsync(dir) ? -2 : 0;
fail:
	close(fd);
	unlinkat(dir, tmp, 0);
	return -1;
}

int mesh_store_remove(int dir, const char *name)
{
	if (unlinkat(dir, name, 0) && errno != ENOENT)
		return -1;
	return fsync(dir) ? -1 : 0;
}

uint64_t mesh_now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

unsigned int mesh_jitter(unsigned int max_ms)
{
	unsigned int n;

	if (!max_ms || getrandom(&n, sizeof(n), GRND_NONBLOCK) != sizeof(n))
		return 0;
	return n % max_ms;
}

void mesh_hash_hex(const void *data, size_t len, char out[MESH_ID_LEN + 1])
{
	static const char alphabet[] = "0123456789abcdef";
	unsigned char digest[32];

	mbedtls_sha256(data, len, digest, 0);
	for (size_t n = 0; n < sizeof(digest); n++) {
		out[n * 2] = alphabet[digest[n] >> 4];
		out[n * 2 + 1] = alphabet[digest[n] & 15];
	}
	out[MESH_ID_LEN] = 0;
}
