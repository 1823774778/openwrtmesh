# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep the wifiroam backend, LuCI frontend and rpcd contracts in sync without a router."""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UC = ROOT / "package/wifiroam/files/wifiroamd.uc"
LUCI = ROOT / "luci-app-wifiroam"
MENU = LUCI / "root/usr/share/luci/menu.d/luci-app-wifiroam.json"
ACL = LUCI / "root/usr/share/rpcd/acl.d/luci-app-wifiroam.json"
VIEW_ROOT = LUCI / "htdocs/luci-static/resources/view"
VIEWS = VIEW_ROOT / "wifiroam"
COMMON = LUCI / "htdocs/luci-static/resources/wifiroam/common.js"

# The 31 methods published by wifiroamd.uc api_obj and granted by ACL.
READ = {
    "status", "topology", "client_detail", "get_roam", "precheck", "discover",
    "roam_log", "get_backhaul", "appctl_status", "offload_status", "get_wifi", "get_guest",
    "device_history", "ban_list",
}
WRITE = {
    "set_alias", "set_role", "set_roam", "set_backhaul", "pair", "unpair",
    "set_appctl", "set_app_policy", "set_offload", "set_wifi", "set_guest",
    "set_pin", "ungroup_device", "wake", "set_ban", "ban_set", "ban_del",
}
ALL_METHODS = READ | WRITE


class ContractTests(unittest.TestCase):
    def test_backend_publishes_every_method(self):
        src = UC.read_text(encoding="utf-8")
        for method in ALL_METHODS:
            with self.subTest(method=method):
                self.assertRegex(src, rf'\b{re.escape(method)}\s*:\s*\{{')
        self.assertIn('publish("wifiroam"', src)

    def test_acl_matches_backend_methods(self):
        acl = json.loads(ACL.read_text(encoding="utf-8"))
        self.assertEqual(set(acl), {"luci-app-wifiroam"})
        entry = acl["luci-app-wifiroam"]
        self.assertEqual(set(entry["read"]["ubus"]["wifiroam"]), READ)
        self.assertEqual(set(entry["write"]["ubus"]["wifiroam"]), WRITE)
        self.assertEqual(len(entry["read"]["ubus"]["wifiroam"]), len(READ))
        self.assertEqual(len(entry["write"]["ubus"]["wifiroam"]), len(WRITE))

    def test_menu_resolves_to_four_views(self):
        menu = json.loads(MENU.read_text(encoding="utf-8"))
        parent = menu["admin/wifiroam"]
        self.assertIn("luci-app-wifiroam", parent["depends"]["acl"])
        views = {e["action"]["path"] for e in menu.values() if e["action"]["type"] == "view"}
        self.assertEqual(len(views), 4)
        for view in views:
            with self.subTest(view=view):
                self.assertTrue((VIEW_ROOT / f"{view}.js").is_file())

    def test_frontend_only_calls_known_methods(self):
        """Every object:'wifiroam' method used by the UI must exist in the backend."""
        called = set()
        for js in VIEWS.glob("*.js"):
            text = js.read_text(encoding="utf-8")
            # wr.call('method' / wr.safeCall('method'
            called |= set(re.findall(r'wr\.(?:safeCall|call)\(\s*[\'"]([\w]+)[\'"]', text))
        common = COMMON.read_text(encoding="utf-8")
        called |= set(re.findall(r'wr\.(?:safeCall|call)\(\s*[\'"]([\w]+)[\'"]', common))
        self.assertTrue(called)
        unknown = called - ALL_METHODS
        self.assertEqual(unknown, set(), f"frontend calls methods absent in backend: {sorted(unknown)}")

    def test_version_is_synchronised(self):
        src = UC.read_text(encoding="utf-8")
        m = re.search(r'DAEMON_VERSION\s*=\s*(\d+)', src)
        self.assertIsNotNone(m, "DAEMON_VERSION missing in backend")
        version = m.group(1)
        for js in VIEWS.glob("*.js"):
            text = js.read_text(encoding="utf-8")
            if "REQUIRED_VERSION" in text:
                req = re.search(r'REQUIRED_VERSION\s*=\s*(\d+)', text)
                self.assertIsNotNone(req)
                self.assertEqual(req.group(1), version, f"version mismatch in {js.name}")

    def test_default_config_has_no_credentials_and_start_disabled_pairing(self):
        cfg = (ROOT / "package/wifiroam/files/wifiroam.config").read_text(encoding="utf-8")
        self.assertIn("option role 'unconfigured'", cfg)
        self.assertIn("option token ''", cfg)
        self.assertNotIn("PRIVATE KEY", cfg)


if __name__ == "__main__":
    unittest.main()
