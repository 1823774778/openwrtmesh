/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MESH_WIFI_H
#define MESH_WIFI_H

#include <stdbool.h>
#include <stddef.h>

#define MESH_AP_MAX 8
#define MESH_SECTION_MAX 64
#define MESH_IFNAME_MAX 16

struct mesh_profile;

struct mesh_wifi {
	size_t count;
	char sections[MESH_AP_MAX][MESH_SECTION_MAX];
	char ifnames[MESH_AP_MAX][MESH_IFNAME_MAX];
};

/* Synchronous and serialized by the caller; no reloads are issued here. */
int mesh_wifi_precheck(struct mesh_wifi *w, char *error, size_t len);
int mesh_wifi_stage(struct mesh_wifi *w, const struct mesh_profile *p,
		    char *error, size_t len);
/* -ECANCELED: this single publish attempt did not rename wireless.
 * Other negative results require the caller to retain its recovery journal. */
int mesh_wifi_publish(char *error, size_t len);
/* Restore only this transaction's candidate, or succeed if already original.
 * Leave the backup and candidate in place for retry after a crash. */
int mesh_wifi_restore(char *error, size_t len);
/* Caller must first durably remove any journal that references the backup. */
int mesh_wifi_clear(void);
bool mesh_wifi_pending(void);
/* Read-only comparison; caller's verified interface names are not modified. */
int mesh_wifi_matches(struct mesh_wifi *w, const struct mesh_profile *p);

#endif
