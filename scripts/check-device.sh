#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# 只读诊断：设备上运行，不改任何配置。用于判断 APK 依赖 ABI 是否与本机一致。
set -eu

echo '== 固件 =='
for f in /etc/openwrt_release /etc/openwrt_version; do
	[ -r "$f" ] && { echo "-- $f"; cat "$f"; }
done
echo "uname: $(uname -a)"

echo '== 软件包管理器与架构 =='
if command -v apk >/dev/null 2>&1; then
	echo "apk: $(command -v apk)"
	apk --print-arch 2>&1 || true
else
	echo 'apk: 未安装（24.10 及更早）'
fi
if command -v opkg >/dev/null 2>&1; then
	opkg print-architecture 2>&1 || true
fi

echo '== 已安装的库 ABI =='
if command -v apk >/dev/null 2>&1; then
	apk list --installed 'libubox*' 'libubus*' 'libblobmsg-json*' 'libuci*' 'libmbedtls*' 2>&1 || true
else
	opkg list-installed 'libubox*' 2>&1 || true
	opkg list-installed 'libubus*' 2>&1 || true
	opkg list-installed 'libblobmsg-json*' 2>&1 || true
	opkg list-installed 'libuci*' 2>&1 || true
	opkg list-installed 'libmbedtls*' 2>&1 || true
fi

echo '== wpad / hostapd =='
if command -v apk >/dev/null 2>&1; then
	apk list --installed 'wpad*' 'hostapd*' 2>&1 || true
else
	opkg list-installed 'wpad*' 2>&1 || true
	opkg list-installed 'hostapd*' 2>&1 || true
fi

echo '== 软件源 =='
for f in /etc/apk/repositories /etc/apk/repositories.d/*.list /etc/opkg/distfeeds.conf /etc/opkg/customfeeds.conf; do
	[ -r "$f" ] && { echo "-- $f"; cat "$f"; }
done

echo '== 元件版本标记 =='
for p in libubox libubus libblobmsg-json libuci libmbedtls; do
	if [ -r "/usr/lib/opkg/info/$p.control" ]; then
		echo "-- $p (opkg)"; grep -E '^(Package|Version):' "/usr/lib/opkg/info/$p.control" || true
	fi
done
