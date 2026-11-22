# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep the package, JavaScript and rpcd contracts in sync without a router."""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "package/meshd/src"
LUCI = ROOT / "luci-app-meshd"
RO = {
    "status", "topology", "link_quality", "client_list", "roam_config",
    "config_status", "diagnose", "upgrade_status", "event_log",
}
RW = {"enroll", "remove_node", "config_apply", "config_rollback"}
DEFERRED = {
    "set_role", "set_roam", "set_backhaul", "set_guest", "set_qos", "set_parental",
    "upgrade_node", "optimize_topology",
}


class ContractTests(unittest.TestCase):
    def test_ubus_implementation_and_limited_acl(self):
        rpc = (SRC / "rpc.c").read_text(encoding="utf-8")
        methods = re.findall(r'UBUS_METHOD(?:_NOARG)?\("(\w+)",\s*(\w+)', rpc)
        self.assertEqual(len(methods), 21)
        self.assertEqual({name for name, _ in methods}, RO | RW | DEFERRED)
        self.assertEqual({name for name, handler in methods if handler == "unsupported"}, DEFERRED)
        acl = json.loads((LUCI / "root/usr/share/rpcd/acl.d/luci-app-meshd.json").read_text())
        self.assertEqual(set(acl), {"luci-app-meshd"})
        for scope, expected in {"read": RO, "write": RW}.items():
            self.assertEqual(acl["luci-app-meshd"][scope], {"ubus": {"meshd": acl["luci-app-meshd"][scope]["ubus"]["meshd"]}})
            allowed = acl["luci-app-meshd"][scope]["ubus"]["meshd"]
            self.assertEqual(len(allowed), len(expected))
            self.assertEqual(set(allowed), expected)

    def test_menu_resolves_to_six_views(self):
        menu = json.loads((LUCI / "root/usr/share/luci/menu.d/luci-app-meshd.json").read_text())
        parent = menu["admin/network/mesh"]
        self.assertIn("luci-app-meshd", parent["depends"]["acl"])
        views = {entry["action"]["path"] for entry in menu.values() if entry["action"]["type"] == "view"}
        self.assertEqual(len(views), 6)
        for view in views:
            with self.subTest(view=view):
                path = LUCI / "htdocs/luci-static/resources/view" / f"{view}.js"
                self.assertTrue(path.is_file())

    def test_disabled_default_does_not_ship_credentials(self):
        cfg = (ROOT / "package/meshd/files/meshd.config").read_text()
        core = cfg.split("config mesh 'roam'", 1)[0]
        self.assertIn("option enabled '0'", core)
        self.assertIn("option role 'candidate'", core)
        self.assertIn("option mtls '1'", cfg)
        self.assertIn("option allow_wps '0'", cfg)
        self.assertNotIn("PRIVATE KEY", cfg)
        self.assertNotIn("BEGIN CERTIFICATE", cfg)

    def test_no_shell_and_no_password_reply(self):
        for source in SRC.glob("*.c"):
            self.assertNotRegex(source.read_text(encoding="utf-8"), r'\b(?:system|popen)\s*\(')
        rpc = (SRC / "rpc.c").read_text(encoding="utf-8")
        self.assertNotRegex(rpc, r'blobmsg_add_string\([^;]*"(?:key|password|psk)"')


if __name__ == "__main__":
    unittest.main()
