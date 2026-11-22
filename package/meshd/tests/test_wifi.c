/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Build and run against a path-rewritten wifi.c, as in README-wifi.md. */
#include "wifi.h"
#include "protocol.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MESH_WIFI_TEST_ROOT
#error "Use the isolated setup in README-wifi.md"
#endif

/* Defined only by the fixture build, so this cannot link to stock wifi.o. */
extern const char mesh_wifi_test_link_root[];
#define ROOT MESH_WIFI_TEST_ROOT "/"
#define CONF ROOT "config/wireless"
#define BACKUP ROOT "state/wireless.backup"
#define STAGE ROOT "state/wireless.stage"
#define STATUS ROOT "status"
#define CLI_EDIT ROOT "delta/wireless"
#define LUCI_EDIT ROOT "rpcd/uci-example/wireless"
#define LUCI_SNAPSHOT ROOT "rpcd/snapshot-files/wireless"
#define OVERRIDE ROOT "override/wireless"

static const char original[] =
	"# Preserve original byte-for-byte on restore\n"
	"config wifi-device 'radio0'\n"
	" option type 'mac80211'\n"
	" option band '5g'\n"
	"config wifi-iface 'client_ap'\n"
	" option mode 'ap'\n"
	" option ifname 'mesh-ap.1'\n"
	" option device 'radio0'\n"
	" option encryption 'psk2+ccmp'\n"
	" option network 'lan'\n"
	" option ieee80211w '2'\n"
	" option ssid 'old'\n"
	" option key 'dummy-original'\n"
	" option wps_pushbutton '1'\n"
	" option wps_label '1'\n"
	" option ext_registrar '1'\n"
	" option wps_pin '12345678'\n"
	"config wifi-iface 'uplink'\n"
	" option mode 'sta'\n"
	" option wds '1'\n"
	" option device 'radio0'\n"
	" option ssid 'preserve-backhaul'\n"
	" list network 'backhaul'\n"
	" list network 'other'\n"
	"config wifi-iface\n"
	" option mode 'monitor'\n"
	" option device 'radio0'\n";

static const char installed[] = "Package: wpad-mbedtls\nStatus: install user installed\n\n";
static const char delta[] = "wireless.client_ap.wds='1'\n";
static const char conflict[] = "# concurrent administrator edit\n";

static void put(const char *path, const char *data)
{
	FILE *f = fopen(path, "w");

	assert(f);
	assert(fputs(data, f) >= 0);
	assert(!fclose(f));
}

static void get(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "r");
	size_t n;

	assert(f);
	n = fread(buf, 1, cap, f);
	assert(n < cap && !ferror(f));
	buf[n] = '\0';
	assert(!fclose(f));
}

static void equal_file(const char *path, const char *expected)
{
	char buf[8192];

	get(path, buf, sizeof(buf));
	assert(!strcmp(buf, expected));
}

static void private_file(const char *path)
{
	struct stat st;

	assert(!lstat(path, &st) && S_ISREG(st.st_mode));
	assert(st.st_uid == 0 && (st.st_mode & 07777) == 0600);
}

static void ok(int result, const char *reason)
{
	if (result)
		fprintf(stderr, "unexpected failure: %s\n", reason);
	assert(!result);
}

static void rejected_config(struct mesh_wifi *w, const char *added)
{
	char image[4096], error[128];
	int n = snprintf(image, sizeof(image), "%s%s", original, added);

	assert(n > 0 && (size_t)n < sizeof(image));
	put(CONF, image);
	assert(mesh_wifi_precheck(w, error, sizeof(error)) < 0);
	assert(!mesh_wifi_pending());
}

int main(void)
{
	struct mesh_wifi w = { .count = 1, .sections = { "client_ap" } };
	struct mesh_wifi uplink = { .count = 1, .sections = { "uplink" } };
	struct mesh_profile p = {
		.version = 1, .flags = MESH_K_BEACON | MESH_FT,
		.ssid = "Cafe's #1", .key = "dummy'$#\\ ciphertext",
	};
	char error[128], candidate[8192];

	if (geteuid() || strcmp(mesh_wifi_test_link_root, MESH_WIFI_TEST_ROOT) ||
	    strncmp(ROOT, "/tmp/meshd-wifi-test.", sizeof("/tmp/meshd-wifi-test.") - 1)) {
		fputs("run only in the isolated root-owned fixture\n", stderr);
		return 2;
	}
	umask(0077);
	put(CONF, original);

	assert(mesh_wifi_precheck(&w, error, sizeof(error)) < 0); /* missing opkg database */
	put(STATUS, "Package: wpad-basic-mbedtls\nStatus: install user installed\n");
	assert(mesh_wifi_precheck(&w, error, sizeof(error)) < 0);
	put(STATUS, "Package: wpad-mini\nStatus: install user installed\n");
	assert(mesh_wifi_precheck(&w, error, sizeof(error)) < 0);
	put(STATUS, installed);

	put(CLI_EDIT, delta);
	assert(mesh_wifi_precheck(&w, error, sizeof(error)) < 0);
	equal_file(CLI_EDIT, delta);
	put(CLI_EDIT, ""); /* libuci commits may leave an empty delta file */
	put(LUCI_EDIT, delta);
	assert(mesh_wifi_precheck(&w, error, sizeof(error)) < 0);
	equal_file(LUCI_EDIT, delta);
	assert(!unlink(LUCI_EDIT));
	put(LUCI_SNAPSHOT, original);
	assert(mesh_wifi_precheck(&w, error, sizeof(error)) < 0);
	assert(!unlink(LUCI_SNAPSHOT));
	put(OVERRIDE, original);
	assert(mesh_wifi_precheck(&w, error, sizeof(error)) < 0);
	assert(!unlink(OVERRIDE));

	rejected_config(&w, "config wifi-iface 'client_ap'\n option disabled '1'\n");
	rejected_config(&w, "config wifi-iface 'client_ap'\n option wds '1'\n");
	rejected_config(&w, "config wifi-iface 'client_ap'\n option encryption 'sae-mixed'\n");
	rejected_config(&w, "config wifi-iface 'client_ap'\n option ifname 'too-long-mesh-ap'\n");
	rejected_config(&w, "config wifi-iface 'client_ap'\n option wpa_psk_file '/tmp/dummy'\n");
	rejected_config(&w, "config wifi-station 'custom'\n option key 'dummy-station'\n");
	rejected_config(&w, "config wifi-vlan 'tagged'\n option iface 'client_ap'\n");
	put(CONF, original);
	ok(mesh_wifi_precheck(&w, error, sizeof(error)), error);
	assert(!strcmp(w.ifnames[0], "mesh-ap.1"));
	assert(mesh_wifi_precheck(&uplink, error, sizeof(error)) < 0);
	assert(mesh_wifi_matches(&w, &p) < 0);
	{
		struct mesh_profile invalid = p;

		strcpy(invalid.key, "short");
		assert(mesh_wifi_stage(&w, &invalid, error, sizeof(error)) < 0);
		assert(!mesh_wifi_pending());
	}

	ok(mesh_wifi_stage(&w, &p, error, sizeof(error)), error);
	assert(mesh_wifi_pending());
	private_file(BACKUP);
	private_file(STAGE);
	equal_file(BACKUP, original);
	equal_file(CONF, original);
	get(STAGE, candidate, sizeof(candidate));
	ok(mesh_wifi_publish(error, sizeof(error)), error);
	equal_file(CONF, candidate);
	private_file(CONF);
	assert(mesh_wifi_matches(&w, &p) == 0); /* includes quote/backslash round-trip */
	assert(!unlink(STATUS));
	assert(mesh_wifi_matches(&w, &p) == 0); /* no package check on compare */
	put(STATUS, installed);

	assert(strstr(candidate, "ssid 'preserve-backhaul'") && strstr(candidate, "wds '1'"));
	assert(strstr(candidate, "list network 'backhaul'") && strstr(candidate, "list network 'other'"));
	assert(strstr(candidate, "config wifi-iface\n")); /* anonymous section preserved */
	assert(strstr(candidate, "encryption 'psk2+ccmp'") && strstr(candidate, "ieee80211w '2'"));
	assert(strstr(candidate, "rrm_neighbor_report '0'") && strstr(candidate, "rrm_beacon_report '1'"));
	assert(strstr(candidate, "bss_transition '0'") && strstr(candidate, "ieee80211r '1'"));
	assert(strstr(candidate, "mobility_domain '7e46'") && strstr(candidate, "ft_psk_generate_local '1'"));
	assert(strstr(candidate, "ft_over_ds '0'") && strstr(candidate, "reassociation_deadline '2000'"));
	assert(strstr(candidate, "wps_pushbutton '0'") && strstr(candidate, "wps_label '0'"));
	assert(strstr(candidate, "ext_registrar '0'") && !strstr(candidate, "wps_pin"));

	put(CONF, conflict);
	assert(mesh_wifi_restore(error, sizeof(error)) < 0);
	equal_file(CONF, conflict);
	put(CONF, candidate);
	ok(mesh_wifi_restore(error, sizeof(error)), error);
	equal_file(CONF, original);
	ok(mesh_wifi_restore(error, sizeof(error)), error);
	assert(mesh_wifi_pending());
	assert(!mesh_wifi_clear());
	assert(!mesh_wifi_pending());

	ok(mesh_wifi_stage(&w, &p, error, sizeof(error)), error);
	put(LUCI_EDIT, delta);
	assert(mesh_wifi_publish(error, sizeof(error)) == -ECANCELED);
	equal_file(CONF, original);
	equal_file(LUCI_EDIT, delta);
	assert(!unlink(LUCI_EDIT));
	put(CONF, conflict);
	assert(mesh_wifi_publish(error, sizeof(error)) == -ECANCELED);
	equal_file(CONF, conflict);
	assert(!mesh_wifi_clear());
	assert(!mesh_wifi_clear());
	puts("wifi transaction smoke passed");
	return 0;
}
