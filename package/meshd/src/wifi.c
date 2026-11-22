/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "wifi.h"
#include "protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <uci.h>

#define CONFIG_DIR "/etc/config"
#define STATE_DIR "/etc/meshd"
#define OPKG_STATUS "/usr/lib/opkg/status"
#define APK_ROOT "/lib/apk"
#define APK_BIN "/usr/bin/apk"
/* Paths stored in the package payload list, independent of filesystem reads. */
#define APK_PAYLOAD_DIR "/usr/sbin"
#define HOSTAPD_DIR APK_PAYLOAD_DIR
#define WIRELESS "wireless"
#define BACKUP "wireless.backup"
#define STAGING "wireless.stage"
#define TMP_PREFIX ".meshd-wireless-"
#define TMP_SUFFIX "/" TMP_PREFIX "XXXXXX"
#define WIRELESS_MAX (1024U * 1024U)
#define OPKG_STATUS_MAX (4U * 1024U * 1024U)
#define APK_LIST_MAX 4096U

static const char *const full_providers[] = {
	"wpad", "wpad-openssl", "wpad-wolfssl", "wpad-mbedtls",
	"wpad-mesh-openssl", "wpad-mesh-wolfssl", "wpad-mesh-mbedtls",
	"hostapd", "hostapd-openssl", "hostapd-wolfssl", "hostapd-mbedtls",
};

/* Actual conflicting providers in OpenWrt 25.12, not virtual package names. */
static const char *const reduced_providers[] = {
	"wpad-basic", "wpad-basic-openssl", "wpad-basic-wolfssl", "wpad-basic-mbedtls",
	"wpad-mini", "hostapd-basic", "hostapd-basic-openssl", "hostapd-basic-wolfssl",
	"hostapd-basic-mbedtls", "hostapd-mini",
};

static const char cannot_verify_hostapd[] = "cannot verify installed hostapd package";
static const char full_hostapd_required[] = "full wpad or hostapd required";

struct wifi_image {
	struct stat st;
	char *data;
	size_t len;
};

struct wifi_tmp {
	char path[64]; /* /proc/self/fd/<dir>/.meshd-wireless-XXXXXX */
	int fd;
};

static int fail(char *error, size_t len, int code, const char *reason)
{
	if (error && len)
		snprintf(error, len, "%s", reason);
	return code;
}

static bool ascii_alnum(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9');
}

static bool name_valid(const char *name, size_t max, bool ifname)
{
	size_t n;

	if (!name)
		return false;
	n = strnlen(name, max);
	if (!n || n == max || (ifname &&
	    (!strcmp(name, ".") || !strcmp(name, ".."))))
		return false;
	for (size_t i = 0; i < n; i++) {
		if (!ascii_alnum(name[i]) && name[i] != '_' &&
		    (!ifname || (name[i] != '-' && name[i] != '.')))
			return false;
	}
	return true;
}

static bool targets_valid(const struct mesh_wifi *w)
{
	if (!w || !w->count || w->count > MESH_AP_MAX)
		return false;
	for (size_t i = 0; i < w->count; i++) {
		if (!name_valid(w->sections[i], MESH_SECTION_MAX, false))
			return false;
		for (size_t j = 0; j < i; j++) {
			if (!strcmp(w->sections[i], w->sections[j]))
				return false;
		}
	}
	return true;
}

static bool option_is(struct uci_context *ctx, struct uci_section *s,
		      const char *name, const char *value)
{
	const char *found = uci_lookup_option_string(ctx, s, name);

	return found && !strcmp(found, value);
}

static bool zero_or_absent(struct uci_context *ctx, struct uci_section *s,
			   const char *name)
{
	struct uci_option *o = uci_lookup_option(ctx, s, name);

	return !o || (o->type == UCI_TYPE_STRING &&
		      (!strcmp(o->v.string, "0") || !strcmp(o->v.string, "false")));
}

static const char *inspect_targets(struct uci_context *ctx, struct uci_package *pkg,
				   const struct mesh_wifi *w, struct uci_section **targets,
				   char names[MESH_AP_MAX][MESH_IFNAME_MAX])
{
	static const char *const incompatible[] = {
		"hostapd_bss_options", "wds_bridge", "wpa_psk_file", "sae_password_file",
		"network_vlan", "vlan_file", "vlan_bridge", "vlan_tagged_interface", "macfile",
		"multi_ap_backhaul_ssid", "multi_ap_backhaul_key",
		"auth_server", "auth_secret", "acct_server", "acct_secret", "dae_client",
		"eap_type", "eap_user_file", "radius_server_clients", "r0kh", "r1kh",
		"r1_key_holder", "owe_transition_ssid", "owe_transition_bssid",
		"owe_transition_ifname",
	};

	for (size_t i = 0; i < w->count; i++) {
		struct uci_section *s = uci_lookup_section(ctx, pkg, w->sections[i]);
		struct uci_section *radio;
		struct uci_element *e;
		const char *ifname, *device;

		if (!s || s->anonymous || strcmp(s->type, "wifi-iface") ||
		    !option_is(ctx, s, "mode", "ap"))
			return "named AP required";
		if (!zero_or_absent(ctx, s, "disabled") ||
		    !zero_or_absent(ctx, s, "start_disabled") ||
		    !zero_or_absent(ctx, s, "default_disabled"))
			return "AP disabled";
		if (!zero_or_absent(ctx, s, "wds") || !zero_or_absent(ctx, s, "multi_ap") ||
		    !zero_or_absent(ctx, s, "apup"))
			return "AP also used for backhaul";
		if (!option_is(ctx, s, "encryption", "psk2") &&
		    !option_is(ctx, s, "encryption", "psk2+ccmp"))
			return "WPA2 Personal required";
		if (!zero_or_absent(ctx, s, "dynamic_vlan") ||
		    !zero_or_absent(ctx, s, "per_sta_vif") ||
		    !zero_or_absent(ctx, s, "eap_server") ||
		    !zero_or_absent(ctx, s, "ppsk") ||
		    !zero_or_absent(ctx, s, "mbo") || !zero_or_absent(ctx, s, "osen") ||
		    !zero_or_absent(ctx, s, "iw_enabled") || !zero_or_absent(ctx, s, "hs20"))
			return "AP authentication or VLAN unsupported";
		for (size_t j = 0; j < sizeof(incompatible) / sizeof(incompatible[0]); j++) {
			if (uci_lookup_option(ctx, s, incompatible[j]))
				return "AP has custom security options";
		}

		device = uci_lookup_option_string(ctx, s, "device");
		radio = device ? uci_lookup_section(ctx, pkg, device) : NULL;
		if (!radio || strcmp(radio->type, "wifi-device") ||
		    !option_is(ctx, radio, "type", "mac80211") ||
		    option_is(ctx, radio, "band", "6g") ||
		    option_is(ctx, radio, "band", "60g") ||
		    option_is(ctx, radio, "hwmode", "11ad") || option_is(ctx, radio, "hwmode", "ad"))
			return "unsupported AP radio";
		if (!zero_or_absent(ctx, radio, "disabled"))
			return "AP radio disabled";
		if (uci_lookup_option(ctx, radio, "hostapd_options"))
			return "radio has custom hostapd options";

		ifname = uci_lookup_option_string(ctx, s, "ifname");
		if (!name_valid(ifname, MESH_IFNAME_MAX, true))
			return "explicit valid ifname required";
		uci_foreach_element(&pkg->sections, e) {
			struct uci_section *other = uci_to_section(e);

			if (other == s)
				continue;
			if (!strcmp(other->type, "wifi-iface")) {
				if (option_is(ctx, other, "ifname", ifname))
					return "duplicate wireless ifname";
			} else if (!strcmp(other->type, "wifi-vlan") ||
				   !strcmp(other->type, "wifi-station")) {
				const char *iface = uci_lookup_option_string(ctx, other, "iface");

				/* No scalar iface applies this section to every named AP. */
				if (!iface || !strcmp(iface, s->e.name))
					return "AP has VLAN or station configuration";
			}
		}
		targets[i] = s;
		strcpy(names[i], ifname);
	}
	return NULL;
}

static int configure_ap(struct uci_context *ctx, struct uci_section *s,
			const struct mesh_profile *p, bool update)
{
	const unsigned int flags = p->flags;
	const bool ft = flags & MESH_FT;
	char domain[5];
	const struct {
		const char *name;
		const char *value;
		bool ft_only;
	} settings[] = {
		{ "ssid", p->ssid, false },
		{ "key", p->key, false },
		{ "ieee80211k", flags & (MESH_K_NEIGHBOR | MESH_K_BEACON) ? "1" : "0", false },
		{ "rrm_neighbor_report", flags & MESH_K_NEIGHBOR ? "1" : "0", false },
		{ "rrm_beacon_report", flags & MESH_K_BEACON ? "1" : "0", false },
		{ "bss_transition", flags & MESH_V_BTM ? "1" : "0", false },
		{ "ieee80211r", ft ? "1" : "0", false },
		/* 24.10 emits WPS state/PIN only if a config method is enabled. */
		{ "wps_pushbutton", "0", false },
		{ "wps_label", "0", false },
		{ "ext_registrar", "0", false },
		{ "wps_pbc_in_m1", "0", false },
		{ "wps_pin", NULL, false },
		{ "mobility_domain", domain, true },
		{ "ft_psk_generate_local", "1", true },
		{ "ft_over_ds", "0", true },
		{ "reassociation_deadline", "2000", true },
	};

	if (ft) {
		unsigned char input[3 + sizeof(p->ssid) - 1] = { 'm', 'd', ':' };
		unsigned char hash[32];
		size_t n = strlen(p->ssid);

		memcpy(input + 3, p->ssid, n);
		if (mbedtls_sha256(input, n + 3, hash, 0))
			return -EIO;
		snprintf(domain, sizeof(domain), "%02x%02x",
			 (unsigned int)hash[0], (unsigned int)hash[1]);
	}

	for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
		struct uci_ptr ptr = { .p = s->package, .s = s, .option = settings[i].name,
				       .value = settings[i].value ? settings[i].value : "" };

		if (settings[i].ft_only && !ft)
			continue;
		if (settings[i].value ? option_is(ctx, s, ptr.option, ptr.value) :
		    !uci_lookup_option(ctx, s, ptr.option))
			continue;
		if (!update || uci_set(ctx, &ptr) != UCI_OK)
			return -EINVAL;
	}
	return 0;
}

/* Identify package stanzas, not strings in descriptions/Provides/Conffiles. */
static int provider(const char *name)
{
	for (size_t i = 0; i < sizeof(full_providers) / sizeof(full_providers[0]); i++) {
		if (!strcmp(name, full_providers[i]))
			return 1;
	}
	if (!strncmp(name, "wpad-", 5) ||
	    (!strncmp(name, "hostapd-", 8) && strcmp(name, "hostapd-common") &&
	     strcmp(name, "hostapd-utils")))
		return -1;
	return 0;
}

static bool same_stat(const struct stat *a, const struct stat *b);

static const char *check_hostapd_opkg(void)
{
	const char *reason = cannot_verify_hostapd;
	struct stat st, end;
	char *line = NULL;
	size_t cap = 0, total = 0;
	unsigned int full = 0, unsupported = 0;
	int kind = 0;
	bool installed = false, have_status = false, have_package = false;
	ssize_t n;
	FILE *fp;
	int fd = open(OPKG_STATUS, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);

	if (fd < 0)
		return reason;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid ||
	    (st.st_mode & 022) || st.st_size <= 0 || st.st_size > OPKG_STATUS_MAX ||
	    !(fp = fdopen(fd, "r"))) {
		close(fd);
		return reason;
	}

	for (;;) {
		n = getline(&line, &cap, fp);
		if (n >= 0) {
			total += (size_t)n;
			if (total > OPKG_STATUS_MAX || memchr(line, 0, (size_t)n))
				goto done;
			line[strcspn(line, "\r\n")] = '\0';
		} else if (!feof(fp) || ferror(fp)) {
			goto done;
		}
		if (n < 0 || !*line) {
			if (kind && !have_status)
				goto done;
			if (kind && installed) {
				if (kind > 0)
					full++;
				else
					unsupported++;
			}
			if (n < 0)
				break;
			kind = 0;
			have_status = have_package = installed = false;
		} else if (!strncmp(line, "Package:", 8)) {
			const char *value = line + 8;

			if (have_package)
				goto done;
			value += strspn(value, " \t");
			if (!*value || strpbrk(value, " \t"))
				goto done;
			have_package = true;
			kind = provider(value);
		} else if (!strncmp(line, "Status:", 7)) {
			char state[32], extra;

			if (have_status || sscanf(line + 7, "%*s %*s %31s %c", state, &extra) != 1)
				goto done;
			have_status = true;
			installed = !strcmp(state, "installed");
		}
	}
	if (lstat(OPKG_STATUS, &end) || !same_stat(&st, &end))
		goto done;
	if (unsupported)
		reason = full_hostapd_required;
	else if (full == 1 && access(HOSTAPD_DIR "/hostapd", X_OK) == 0)
		reason = NULL;

done:
	free(line);
	fclose(fp);
	return reason;
}

static int open_dir_at(int dir, const char *path, bool private);

/* APK installs this list as an ordinary package-owned payload file (and
 * removes it with that payload). OpenWrt writes one absolute file or link
 * path per line; none of these hostapd/wpad package names has an ABI suffix. */
static int apk_provider(int dir, const char *pkg, bool wpad)
{
	char name[32], buf[APK_LIST_MAX];
	struct stat st, end;
	bool have_hostapd = false, have_wpad = false;
	size_t pos = 0;
	int fd, result = -1;
	int len = snprintf(name, sizeof(name), "%s.list", pkg);

	if (len < 0 || (size_t)len >= sizeof(name))
		return -1;
	fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return errno == ENOENT ? 0 : -1;
	/* APK lists may be 0644; only root may write them, and we never do. */
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid ||
	    (st.st_mode & 022) || st.st_size <= 0 || st.st_size >= (off_t)sizeof(buf))
		goto out;
	/* Read at most the limit even if the file grows after fstat. */
	while (pos < sizeof(buf)) {
		ssize_t n = read(fd, buf + pos, sizeof(buf) - pos);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			goto out;
		if (!n)
			break;
		pos += (size_t)n;
	}
	if (pos != (size_t)st.st_size || memchr(buf, '\0', pos) || memchr(buf, '\r', pos))
		goto out;
	for (char *line = buf, *limit = buf + pos; line < limit;) {
		char *eol = memchr(line, '\n', (size_t)(limit - line));

		if (!eol || eol <= line + 1 || *line != '/')
			goto out;
		*eol = '\0';
		if (!strcmp(line, APK_PAYLOAD_DIR "/hostapd")) {
			if (have_hostapd)
				goto out;
			have_hostapd = true;
		} else if (!strcmp(line, APK_PAYLOAD_DIR "/wpad")) {
			if (have_wpad)
				goto out;
			have_wpad = true;
		}
		line = eol + 1;
	}
	if (!have_hostapd || have_wpad != wpad ||
	    fstatat(dir, name, &end, AT_SYMLINK_NOFOLLOW) || !same_stat(&st, &end))
		goto out;
	result = 1;

out:
	close(fd);
	return result;
}

static bool wpad_package(const char *pkg)
{
	return !strncmp(pkg, "wpad", sizeof("wpad") - 1);
}

static bool apk_hostapd_executable(bool wpad)
{
	int dir = open_dir_at(AT_FDCWD, HOSTAPD_DIR, false);
	struct stat st;
	bool ok = false;

	if (dir < 0)
		return false;
	if (wpad) {
		char target[sizeof("wpad")];
		ssize_t n = readlinkat(dir, "hostapd", target, sizeof(target));

		/* The upstream multicall executable is reached by this exact link. */
		if (n != (ssize_t)(sizeof(target) - 1) || memcmp(target, "wpad", sizeof(target) - 1))
			goto out;
	}
	ok = !fstatat(dir, wpad ? "wpad" : "hostapd", &st, AT_SYMLINK_NOFOLLOW) &&
	     S_ISREG(st.st_mode) && !st.st_uid && !(st.st_mode & 022) &&
	     faccessat(dir, "hostapd", X_OK, 0) == 0;
out:
	close(dir);
	return ok;
}

static const char *check_hostapd_apk(int root)
{
	const char *reason = cannot_verify_hostapd, *full = NULL;
	int dir = open_dir_at(root, "packages", false);
	struct stat before, after;

	if (dir < 0)
		return reason;
	if (fstat(dir, &before))
		goto out;
	for (size_t i = 0; i < sizeof(reduced_providers) / sizeof(reduced_providers[0]); i++) {
		const char *pkg = reduced_providers[i];
		int rc = apk_provider(dir, pkg, wpad_package(pkg));

		if (rc < 0)
			goto out;
		if (rc > 0) {
			reason = full_hostapd_required;
			goto out;
		}
	}
	for (size_t i = 0; i < sizeof(full_providers) / sizeof(full_providers[0]); i++) {
		const char *pkg = full_providers[i];
		int rc = apk_provider(dir, pkg, wpad_package(pkg));

		if (rc < 0 || (rc > 0 && full))
			goto out;
		if (rc > 0)
			full = pkg;
	}
	if (full && apk_hostapd_executable(wpad_package(full)) &&
	    !fstatat(root, "packages", &after, AT_SYMLINK_NOFOLLOW) && same_stat(&before, &after))
		reason = NULL;

out:
	close(dir);
	return reason;
}

static const char *check_hostapd(void)
{
	const char *reason;
	struct stat st;
	int root = open_dir_at(AT_FDCWD, APK_ROOT, false);

	/* APK takes precedence even if its metadata is incomplete. A stale opkg
	 * stanza must never approve the currently installed basic/mini daemon. */
	if (root < 0) {
		if (errno == ENOENT && lstat(APK_BIN, &st) && errno == ENOENT)
			return check_hostapd_opkg();
		return cannot_verify_hostapd;
	}
	reason = check_hostapd_apk(root);
	close(root);
	return reason;
}

/* 1 if a delta/snapshot/override exists, 0 if absent, -1 on inspection error.
 * libuci may leave an empty wireless delta after a successful commit. */
static int probe(int dir, const char *name, bool allow_empty)
{
	struct stat st;

	if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0)
		return !allow_empty || !S_ISREG(st.st_mode) || st.st_size != 0;
	return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
}

static int scan_deltas(const char *dirpath, const char *prefix)
{
	struct dirent *entry;
	int result = 0;
	DIR *dir = opendir(dirpath);

	if (!dir)
		return errno == ENOENT ? 0 : -1;
	for (;;) {
		char path[sizeof(entry->d_name) + sizeof("/wireless")];

		errno = 0;
		entry = readdir(dir);
		if (!entry) {
			if (errno)
				result = -1;
			break;
		}
		if (strncmp(entry->d_name, prefix, strlen(prefix)))
			continue;
		snprintf(path, sizeof(path), "%s/" WIRELESS, entry->d_name);
		result = probe(dirfd(dir), path, true);
		if (result)
			break;
	}
	closedir(dir);
	return result;
}

static const char *check_edits(void)
{
	int rc = probe(AT_FDCWD, "/tmp/.uci/" WIRELESS, true);

	if (!rc)
		rc = scan_deltas("/tmp", ".uci-");
	if (!rc)
		rc = scan_deltas("/var/run/rpcd", "uci-");
	if (!rc)
		rc = probe(AT_FDCWD, "/var/run/rpcd/snapshot-files/" WIRELESS, false);
	if (!rc)
		rc = probe(AT_FDCWD, UCI_CONF2DIR "/" WIRELESS, false);
	return rc > 0 ? "wireless edits or override pending" :
	       rc < 0 ? "cannot check wireless edits" : NULL;
}

static int open_dir_at(int dir, const char *path, bool private)
{
	struct stat st;
	int fd = openat(dir, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0)
		return fd;
	if (fstat(fd, &st) || st.st_uid || (private ? (st.st_mode & 0777) != 0700 :
						     (st.st_mode & 022) != 0)) {
		close(fd);
		errno = EACCES;
		return -1;
	}
	return fd;
}

static int open_dir(const char *path, bool private)
{
	return open_dir_at(AT_FDCWD, path, private);
}

static bool image_stat_valid(const struct stat *st, bool private)
{
	return S_ISREG(st->st_mode) && st->st_uid == 0 &&
	       (private ? (st->st_mode & 07777) == 0600 : (st->st_mode & 022) == 0) &&
	       st->st_size > 0 && st->st_size <= WIRELESS_MAX;
}

static bool same_stat(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
	       a->st_uid == b->st_uid && a->st_gid == b->st_gid &&
	       a->st_mode == b->st_mode && a->st_size == b->st_size &&
	       a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
	       a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static bool unchanged(int dir, const char *name, const struct wifi_image *image)
{
	struct stat st;

	return fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && same_stat(&st, &image->st);
}

/* Serialize against the original inode where supported by UCI tools. Never
 * wait for the lock in the daemon; callers may try a fresh transaction later. */
static int open_live(int dir, bool write)
{
	int fd = openat(dir, WIRELESS, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);

	if (fd >= 0 && flock(fd, (write ? LOCK_EX : LOCK_SH) | LOCK_NB)) {
		close(fd);
		return -1;
	}
	return fd;
}

static int read_image(int fd, struct wifi_image *image, bool private)
{
	struct stat end;
	size_t pos = 0;

	if (fd < 0 || fstat(fd, &image->st) || !image_stat_valid(&image->st, private))
		return -1;
	image->len = (size_t)image->st.st_size;
	image->data = malloc(image->len);
	if (!image->data)
		return -1;
	while (pos < image->len) {
		ssize_t n = pread(fd, image->data + pos, image->len - pos, (off_t)pos);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		pos += (size_t)n;
	}
	/* libuci reads a text stream; do not silently drop NUL-containing tails. */
	if (memchr(image->data, 0, image->len) || fstat(fd, &end) || !same_stat(&image->st, &end))
		return -1;
	return 0;
}

static int read_private(int dir, const char *name, struct wifi_image *image)
{
	int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	int rc = read_image(fd, image, true);

	if (fd >= 0)
		close(fd);
	return rc;
}

static bool same_image(const struct wifi_image *a, const struct wifi_image *b)
{
	return a->len == b->len && !memcmp(a->data, b->data, a->len);
}

/* Recheck bytes as well as metadata: some router filesystems have coarse
 * timestamps, so size/mtime alone cannot exclude a same-length overwrite. */
static bool live_equals(int fd, int dir, const struct wifi_image *image)
{
	char buf[2048];
	size_t pos = 0;
	bool equal = true;

	while (pos < image->len) {
		size_t room = image->len - pos;
		ssize_t n = pread(fd, buf, room < sizeof(buf) ? room : sizeof(buf), (off_t)pos);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0 || memcmp(buf, image->data + pos, (size_t)n)) {
			equal = false;
			break;
		}
		pos += (size_t)n;
	}
	mbedtls_platform_zeroize(buf, sizeof(buf));
	return equal && unchanged(dir, WIRELESS, image);
}

static int write_image(int fd, const struct wifi_image *image, bool sync);

static struct uci_context *load_wireless(const struct wifi_image *image,
					 struct uci_package **pkg)
{
	struct uci_context *ctx = NULL;
	char path[48];
	int fd = memfd_create("meshd-wireless", MFD_CLOEXEC);

	/* Parse the exact bytes also used as the backup, without reading a
	 * concurrently modified inode or holding a second flock on it. */
	if (fd < 0)
		return NULL;
	if (write_image(fd, image, false) || !(ctx = uci_alloc_context()))
		goto out;
	/* uci_set_savedir() alone retains /tmp/.uci in the delta search path;
	 * use an absolute-path uci_load(), which disables deltas and conf2. */
	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
	ctx->flags = UCI_FLAG_STRICT;
	if (uci_set_savedir(ctx, STATE_DIR "/.uci") != UCI_OK ||
	    uci_load(ctx, path, pkg) != UCI_OK) {
		uci_free_context(ctx);
		ctx = NULL;
	}
out:
	close(fd);
	return ctx;
}

static int make_tmp(struct wifi_tmp *tmp, int dir)
{
	snprintf(tmp->path, sizeof(tmp->path), "/proc/self/fd/%d" TMP_SUFFIX, dir);
	tmp->fd = mkostemp(tmp->path, O_CLOEXEC);
	if (tmp->fd < 0) {
		tmp->path[0] = '\0';
		return -1;
	}
	return fchmod(tmp->fd, 0600);
}

static int drop_tmp(struct wifi_tmp *tmp)
{
	int rc = 0;

	if (tmp->fd >= 0) {
		rc = close(tmp->fd);
		tmp->fd = -1;
	}
	if (tmp->path[0] && (!unlink(tmp->path) || errno == ENOENT))
		tmp->path[0] = '\0';
	return tmp->path[0] ? -1 : rc;
}

static int sync_fd(int fd)
{
	int rc;

	do {
		rc = fsync(fd);
	} while (rc && errno == EINTR);
	return rc;
}

static int write_image(int fd, const struct wifi_image *image, bool sync)
{
	size_t pos = 0;

	while (pos < image->len) {
		ssize_t n = write(fd, image->data + pos, image->len - pos);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		pos += (size_t)n;
	}
	return sync ? sync_fd(fd) : 0;
}

static void free_image(struct wifi_image *image)
{
	if (image->data) {
		mbedtls_platform_zeroize(image->data, image->len);
		free(image->data);
	}
}

/* Confirm export/import preserves the complete UCI tree, including settings
 * of sections this module does not manage. Comments/formatting are not UCI data. */
static bool same_options(const struct uci_list *a, const struct uci_list *b)
{
	const struct uci_list *i = a->next, *j = b->next;

	for (; i != a && j != b; i = i->next, j = j->next) {
		const struct uci_option *left = uci_to_option(list_to_element(i));
		const struct uci_option *right = uci_to_option(list_to_element(j));
		const struct uci_list *x, *y;

		if (strcmp(left->e.name, right->e.name) || left->type != right->type)
			return false;
		if (left->type == UCI_TYPE_STRING) {
			if (strcmp(left->v.string, right->v.string))
				return false;
			continue;
		}
		if (left->type != UCI_TYPE_LIST)
			return false;
		x = left->v.list.next;
		y = right->v.list.next;
		for (; x != &left->v.list && y != &right->v.list; x = x->next, y = y->next) {
			if (strcmp(list_to_element(x)->name, list_to_element(y)->name))
				return false;
		}
		if (x != &left->v.list || y != &right->v.list)
			return false;
	}
	return i == a && j == b;
}

static bool same_sections(const struct uci_package *a, const struct uci_package *b)
{
	const struct uci_list *i = a->sections.next, *j = b->sections.next;

	for (; i != &a->sections && j != &b->sections; i = i->next, j = j->next) {
		const struct uci_section *left = uci_to_section(list_to_element(i));
		const struct uci_section *right = uci_to_section(list_to_element(j));

		if (left->anonymous != right->anonymous || strcmp(left->type, right->type) ||
		    (!left->anonymous && strcmp(left->e.name, right->e.name)) ||
		    !same_options(&left->options, &right->options))
			return false;
	}
	return i == &a->sections && j == &b->sections;
}

static bool export_valid(int fd, const struct uci_package *expected)
{
	struct uci_package *candidate = NULL;
	struct wifi_image image = {0};
	struct uci_context *ctx = NULL;
	bool ok = false;

	if (!read_image(fd, &image, true)) {
		ctx = load_wireless(&image, &candidate);
		ok = ctx && candidate && same_sections(expected, candidate);
	}
	if (ctx)
		uci_free_context(ctx);
	free_image(&image);
	return ok;
}

static int export_wireless(struct uci_context *ctx, struct uci_package *pkg, int fd)
{
	int rc, stream_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	FILE *fp;

	if (stream_fd < 0)
		return -1;
	fp = fdopen(stream_fd, "w");
	if (!fp) {
		close(stream_fd);
		return -1;
	}
	/* No package header, as in uci_commit(); anonymous sections stay anonymous. */
	rc = uci_export(ctx, fp, pkg, false) != UCI_OK || fflush(fp) || ferror(fp);
	if (fclose(fp) || rc || !export_valid(fd, pkg))
		return -1;
	return sync_fd(fd);
}

/* Used by precheck, stage and matches. Stage reuses the same validated UCI
 * object and byte image; there is no separate check then re-read live package. */
static const char *inspect_live(int dir, int fd, struct wifi_image *original,
				struct uci_context **ctx, struct uci_section **targets,
				const struct mesh_wifi *w,
				char names[MESH_AP_MAX][MESH_IFNAME_MAX])
{
	struct uci_package *pkg = NULL;
	const char *reason;

	if (read_image(fd, original, false))
		return "cannot read wireless config";
	*ctx = load_wireless(original, &pkg);
	if (!*ctx || !pkg)
		return "cannot parse wireless config";
	reason = inspect_targets(*ctx, pkg, w, targets, names);
	if (!reason && !unchanged(dir, WIRELESS, original))
		reason = "wireless config changed";
	return reason;
}

static int inspect(const struct mesh_wifi *w, const struct mesh_profile *p,
		   char names[MESH_AP_MAX][MESH_IFNAME_MAX], char *error, size_t len)
{
	struct uci_section *targets[MESH_AP_MAX];
	struct wifi_image original = {0};
	struct uci_context *ctx = NULL;
	const char *reason;
	int dir = -1, fd = -1;

	if (!targets_valid(w))
		return fail(error, len, -EINVAL, "invalid AP selection");
	if (!p && (reason = check_hostapd()))
		return fail(error, len, -EINVAL, reason);
	if ((reason = check_edits()))
		return fail(error, len, -EBUSY, reason);
	if ((dir = open_dir(CONFIG_DIR, false)) >= 0)
		fd = open_live(dir, false);
	reason = inspect_live(dir, fd, &original, &ctx, targets, w, names);
	if (!reason && p) {
		for (size_t i = 0; i < w->count; i++) {
			if ((w->ifnames[i][0] && strncmp(w->ifnames[i], names[i], MESH_IFNAME_MAX)) ||
			    configure_ap(ctx, targets[i], p, false)) {
				reason = "wireless profile differs";
				break;
			}
		}
	}
	if (!reason)
		reason = check_edits();
	if (!reason && !live_equals(fd, dir, &original))
		reason = "wireless config changed";
	if (ctx)
		uci_free_context(ctx);
	free_image(&original);
	if (fd >= 0)
		close(fd);
	if (dir >= 0)
		close(dir);
	return fail(error, len, reason ? -EINVAL : 0, reason ? reason : "");
}

int mesh_wifi_precheck(struct mesh_wifi *w, char *error, size_t len)
{
	char names[MESH_AP_MAX][MESH_IFNAME_MAX] = {{0}};
	int rc = inspect(w, NULL, names, error, len);

	if (!rc)
		memcpy(w->ifnames, names, sizeof(names));
	return rc;
}

int mesh_wifi_matches(struct mesh_wifi *w, const struct mesh_profile *p)
{
	char names[MESH_AP_MAX][MESH_IFNAME_MAX] = {{0}};

	if (!p || !mesh_profile_valid(p))
		return -EINVAL;
	return inspect(w, p, names, NULL, 0);
}

int mesh_wifi_stage(struct mesh_wifi *w, const struct mesh_profile *p,
		    char *error, size_t len)
{
	struct wifi_tmp backup = { .fd = -1 }, stage = { .fd = -1 };
	struct uci_section *targets[MESH_AP_MAX];
	struct wifi_image original = {0};
	struct uci_context *ctx = NULL;
	char names[MESH_AP_MAX][MESH_IFNAME_MAX] = {{0}};
	bool new_stage = false, new_backup = false, differs = false;
	int conf = -1, state = -1, live = -1;
	const char *reason;

	if (!targets_valid(w) || !p || !mesh_profile_valid(p))
		return fail(error, len, -EINVAL, "invalid AP selection or profile");
	if ((reason = check_hostapd()) || (reason = check_edits()))
		return fail(error, len, -EINVAL, reason);

	reason = "wireless state directory unavailable";
	state = open_dir(STATE_DIR, true);
	if (state < 0)
		goto out;
	reason = "wireless transaction already pending";
	if (probe(state, BACKUP, false) || probe(state, STAGING, false))
		goto out;

	if ((conf = open_dir(CONFIG_DIR, false)) >= 0)
		live = open_live(conf, false);
	reason = inspect_live(conf, live, &original, &ctx, targets, w, names);
	if (reason)
		goto out;

	reason = "cannot set wireless options";
	for (size_t i = 0; i < w->count; i++) {
		if (!configure_ap(ctx, targets[i], p, false))
			continue;
		differs = true;
		if (configure_ap(ctx, targets[i], p, true))
			goto out;
	}

	reason = "cannot store wireless staging or backup";
	if (make_tmp(&stage, state) || make_tmp(&backup, state) ||
	    (differs ? export_wireless(ctx, targets[0]->package, stage.fd) :
		       write_image(stage.fd, &original, true)) || write_image(backup.fd, &original, true))
		goto out;
	if ((reason = check_edits()))
		goto out;
	reason = "wireless config changed";
	if (!live_equals(live, conf, &original))
		goto out;

	/* Publish no-clobber private artifacts; the caller may journal only after
	 * both inodes and their directory entries are durable. */
	reason = "cannot store wireless staging or backup";
	if (linkat(AT_FDCWD, backup.path, state, BACKUP, 0))
		goto out;
	new_backup = true;
	if (linkat(AT_FDCWD, stage.path, state, STAGING, 0))
		goto out;
	new_stage = true;
	if (drop_tmp(&stage) || drop_tmp(&backup) || sync_fd(state))
		goto out;
	memcpy(w->ifnames, names, sizeof(names));
	reason = NULL;

out:
	drop_tmp(&backup);
	drop_tmp(&stage);
	if (reason && (new_stage || new_backup)) {
		if (new_stage)
			unlinkat(state, STAGING, 0);
		if (new_backup)
			unlinkat(state, BACKUP, 0);
		sync_fd(state);
	}
	if (ctx)
		uci_free_context(ctx);
	free_image(&original);
	if (live >= 0)
		close(live);
	if (conf >= 0)
		close(conf);
	if (state >= 0)
		close(state);
	return fail(error, len, reason ? -EIO : 0, reason ? reason : "");
}

static int replace_wireless(bool restore, char *error, size_t len)
{
	struct wifi_image current = {0}, backup = {0}, stage = {0};
	struct wifi_tmp tmp = { .fd = -1 };
	int state = -1, conf = -1, live = -1;
	const char *reason;
	bool replaced = false;

	reason = "cannot read wireless backup";
	if ((state = open_dir(STATE_DIR, true)) < 0 || read_private(state, BACKUP, &backup))
		goto out;
	reason = "cannot lock or read wireless config";
	if ((conf = open_dir(CONFIG_DIR, false)) < 0 ||
	    (live = open_live(conf, true)) < 0 || read_image(live, &current, false))
		goto out;

	if (restore && same_image(&current, &backup)) {
		/* A following core reload must not pick up unrelated deltas, even
		 * if nothing needs rewriting. Sync an interrupted restore too. */
		if ((reason = check_edits()))
			goto out;
		reason = "cannot sync restored wireless config";
		if (!sync_fd(live) && !sync_fd(conf) && live_equals(live, conf, &current))
			reason = NULL;
		goto out;
	}
	reason = "cannot read wireless staging";
	if (read_private(state, STAGING, &stage))
		goto out;
	reason = "wireless config changed";
	if (!same_image(&current, restore ? &stage : &backup))
		goto out;

	reason = "cannot prepare wireless replacement";
	if (make_tmp(&tmp, conf) || write_image(tmp.fd, restore ? &backup : &stage, true) ||
	    flock(tmp.fd, LOCK_EX | LOCK_NB))
		goto out;
	if ((reason = check_edits()))
		goto out;
	/* UCI and LuCI do not share a lock spanning their rename. Recheck
	 * bytes and path identity as late as possible. Editors can still
	 * race this final check; avoid parallel edits. */
	reason = "wireless config changed";
	if (!live_equals(live, conf, &current))
		goto out;
	reason = "cannot replace wireless config";
	if (renameat(AT_FDCWD, tmp.path, conf, WIRELESS))
		goto out;
	tmp.path[0] = '\0';
	replaced = true;
	reason = "cannot sync wireless config directory";
	if (!sync_fd(conf))
		reason = NULL;

out:
	drop_tmp(&tmp);
	free_image(&current);
	free_image(&backup);
	free_image(&stage);
	if (live >= 0)
		close(live);
	if (conf >= 0)
		close(conf);
	if (state >= 0)
		close(state);
	return fail(error, len, reason ? (restore || replaced ? -EIO : -ECANCELED) : 0,
		    reason ? reason : "");
}

int mesh_wifi_publish(char *error, size_t len)
{
	return replace_wireless(false, error, len);
}

int mesh_wifi_restore(char *error, size_t len)
{
	return replace_wireless(true, error, len);
}

bool mesh_wifi_pending(void)
{
	struct stat st;
	int state = open_dir(STATE_DIR, true);
	bool pending;

	if (state < 0)
		return false;
	pending = fstatat(state, BACKUP, &st, AT_SYMLINK_NOFOLLOW) == 0 && image_stat_valid(&st, true);
	close(state);
	return pending;
}

/* Remove only files created by our mkostemp template. A power loss can
 * otherwise leave an extra, private copy of a key in either directory. */
static int clear_temps(int fd, bool *changed)
{
	struct dirent *entry;
	int rc = 0, copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	DIR *dir;

	if (copy < 0)
		return -EIO;
	dir = fdopendir(copy);
	if (!dir) {
		close(copy);
		return -EIO;
	}
	for (;;) {
		struct stat st;
		const char *name;
		bool valid = true;
		size_t prefix = sizeof(TMP_PREFIX) - 1;

		errno = 0;
		entry = readdir(dir);
		if (!entry) {
			if (errno)
				rc = -EIO;
			break;
		}
		name = entry->d_name;
		if (strlen(name) != prefix + 6 || strncmp(name, TMP_PREFIX, prefix))
			continue;
		for (size_t i = 0; i < 6; i++)
			valid &= ascii_alnum(name[prefix + i]);
		if (!valid)
			continue;
		if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) {
			if (errno != ENOENT)
				rc = -EIO;
			continue;
		}
		if (!S_ISREG(st.st_mode) || st.st_uid || (st.st_mode & 07777) != 0600)
			continue;
		if (!unlinkat(fd, name, 0))
			*changed = true;
		else if (errno != ENOENT)
			rc = -EIO;
	}
	closedir(dir);
	return rc;
}

int mesh_wifi_clear(void)
{
	static const char *const files[] = { STAGING, BACKUP };
	bool changed = false;
	int state = open_dir(STATE_DIR, true);
	int conf, rc = 0;

	if (state < 0)
		return errno == ENOENT ? 0 : -EIO;
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		if (!unlinkat(state, files[i], 0))
			changed = true;
		else if (errno != ENOENT)
			rc = -EIO;
	}
	if (clear_temps(state, &changed))
		rc = -EIO;
	if (changed && sync_fd(state))
		rc = -EIO;
	close(state);

	conf = open_dir(CONFIG_DIR, false);
	if (conf < 0)
		return errno == ENOENT ? rc : -EIO;
	changed = false;
	if (clear_temps(conf, &changed))
		rc = -EIO;
	if (changed && sync_fd(conf))
		rc = -EIO;
	close(conf);
	return rc;
}
