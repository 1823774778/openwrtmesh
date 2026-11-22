# SPDX-License-Identifier: GPL-2.0-or-later
include $(TOPDIR)/rules.mk

PKG_NAME:=meshd
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_BUILD_FLAGS:=gc-sections

include $(INCLUDE_DIR)/package.mk

MESHD_SOURCES:=core.c rpc.c host.c local.c protocol.c wifi.c transport.c

define Package/meshd
  SECTION:=net
  CATEGORY:=Network
  TITLE:=OpenWrt Mesh control plane
  DEPENDS:=+libubus +libubox +libblobmsg-json +libuci +libmbedtls
endef

define Package/meshd/description
 Native Mesh controller with mutually authenticated node heartbeats and
 versioned wireless configuration for explicitly selected client APs.
endef

define Package/meshd/conffiles
/etc/config/meshd
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/*.c ./src/*.h $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -D_GNU_SOURCE -std=gnu11 \
		-Wall -Wextra -o $(PKG_BUILD_DIR)/meshd \
		$(addprefix $(PKG_BUILD_DIR)/,$(MESHD_SOURCES)) $(TARGET_LDFLAGS) \
		-lubus -lubox -lblobmsg_json -luci -lmbedtls -lmbedx509 -lmbedcrypto
endef

define Package/meshd/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config $(1)/etc/uci-defaults $(1)/lib/upgrade/keep.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/meshd $(1)/usr/sbin/meshd
	$(INSTALL_BIN) ./files/meshd.init $(1)/etc/init.d/meshd
	$(INSTALL_CONF) ./files/meshd.config $(1)/etc/config/meshd
	$(INSTALL_BIN) ./files/meshd.defaults $(1)/etc/uci-defaults/95-meshd
	$(INSTALL_DATA) ./files/meshd.keep $(1)/lib/upgrade/keep.d/meshd
endef

define Package/meshd/postinst
#!/bin/sh
if [ -z "$${IPKG_INSTROOT}" ] && [ -f /etc/uci-defaults/95-meshd ]; then
	/etc/uci-defaults/95-meshd || exit 1
fi
exit 0
endef

$(eval $(call BuildPackage,meshd))
