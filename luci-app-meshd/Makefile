# SPDX-License-Identifier: GPL-2.0-or-later

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-meshd
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-meshd
	SECTION:=luci
	CATEGORY:=LuCI
	SUBMENU:=3. Applications
	TITLE:=LuCI interface for meshd
	DEPENDS:=+meshd +luci-base
	PKGARCH:=all
endef

define Package/luci-app-meshd/description
 LuCI views for Mesh status, node authorization, wireless configuration and diagnostics.
endef

define Build/Prepare
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/luci-app-meshd/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/meshd
	$(INSTALL_DATA) ./htdocs/luci-static/resources/meshd.js $(1)/www/luci-static/resources/
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/meshd/*.js $(1)/www/luci-static/resources/view/meshd/
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/luci-app-meshd.json $(1)/usr/share/luci/menu.d/
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/luci-app-meshd.json $(1)/usr/share/rpcd/acl.d/
endef

define Package/luci-app-meshd/postinst
[ -n "$${IPKG_INSTROOT}" ] || { \
	rm -f /tmp/luci-indexcache.*; \
	/etc/init.d/rpcd reload 2>/dev/null; \
}
exit 0
endef

$(eval $(call BuildPackage,luci-app-meshd))
