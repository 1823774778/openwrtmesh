# Wi-Fi 事务测试（OpenWrt 24.10）

[测试](<test_wifi.c>) 使用真实的 libuci 与 Mbed TLS，但只操作一次性的本地文件夹。不运行设备上的 hostapd/netifd，不发 ubus，也不操作网卡。请在可信的 Linux 测试 VM 内、从仓库根目录运行，不要在生产路由器上运行。需要 `sudo`，因为无线模块明确拒绝非 root 所有的配置与状态目录。

若测试机已装 libuci、libubox 和 Mbed TLS 3.x 开发库，可直接使用本机 C 编译器。否则将 `SDK` 设置为已解包、已构建/stage 上述依赖的官方 OpenWrt 24.10.x x86/64 SDK 的绝对路径；以下命令用该 SDK 的 musl 动态加载器运行。其他架构需要相应的 Linux 执行环境及工具链路径。

```sh
(
    set -eu
    src=package/meshd/src
    test_src=package/meshd/tests/test_wifi.c
    d=$(mktemp -d /tmp/meshd-wifi-test.XXXXXXXX)
    trap 'case "$d" in /tmp/meshd-wifi-test.????????) sudo rm -rf -- "$d" ;; esac' EXIT
    sudo install -d -o root -g root -m 0700 \
        "$d/config" "$d/state" "$d/delta" "$d/rpcd/uci-example" \
        "$d/rpcd/snapshot-files" "$d/override"

    # 仅重写临时副本；不要将测试链接到生产 wifi.o。
    sed -e "s|/etc/config|$d/config|g" \
        -e "s|/etc/meshd|$d/state|g" \
        -e "s|/usr/lib/opkg/status|$d/status|g" \
        -e 's|/usr/sbin/hostapd|/bin/true|g' \
        -e "s|/tmp/\.uci|$d/delta|g" \
        -e "s|scan_deltas(\"/tmp\",|scan_deltas(\"$d\",|g" \
        -e "s|/var/run/rpcd|$d/rpcd|g" \
        -e "s|UCI_CONF2DIR|\"$d/override\"|g" \
        "$src/wifi.c" > "$d/wifi-test-impl.c"
    if grep -Eq '/etc/(config|meshd)|/usr/lib/opkg/status|/usr/sbin/hostapd|/tmp/\.uci|/var/run/rpcd|UCI_CONF2DIR|scan_deltas\("/tmp"' "$d/wifi-test-impl.c"; then
        echo '仍有生产路径；拒绝运行。' >&2
        exit 1
    fi
    printf '\nconst char mesh_wifi_test_link_root[] = "%s";\n' "$d" >> "$d/wifi-test-impl.c"

    if [ -n "${SDK:-}" ]; then
        target="$SDK/staging_dir/target-x86_64_musl"
        toolchain="$SDK/staging_dir/toolchain-x86_64_gcc-13.3.0_musl"
        export PATH="$toolchain/bin:$PATH" STAGING_DIR="$target"
        x86_64-openwrt-linux-musl-gcc -D_GNU_SOURCE -std=gnu11 -Wall -Wextra -Werror \
            "-DMESH_WIFI_TEST_ROOT=\"$d\"" -I"$src" -I"$target/usr/include" \
            "$d/wifi-test-impl.c" "$src/protocol.c" "$test_src" \
            -L"$target/usr/lib" -Wl,-rpath-link,"$target/usr/lib" \
            -luci -lubox -lmbedcrypto -o "$d/test_wifi"
        sudo "$toolchain/lib/ld-musl-x86_64.so.1" \
            --library-path "$target/usr/lib:$toolchain/lib" "$d/test_wifi"
    else
        "${CC:-cc}" -D_GNU_SOURCE -std=gnu11 -Wall -Wextra -Werror \
            "-DMESH_WIFI_TEST_ROOT=\"$d\"" -I"$src" \
            "$d/wifi-test-impl.c" "$src/protocol.c" "$test_src" \
            -luci -lubox -lmbedcrypto -o "$d/test_wifi"
        sudo "$d/test_wifi"
    fi
)
```

预期输出 `wifi transaction smoke passed`。覆盖 AP 与 opkg 拒绝、CLI/LuCI 暂存和覆盖文件、PSK/SSID 转义与漂移检测、FT 参数、WPS 关闭、保留非目标 WDS/网络/PMF 设置、持久备份及字节级回滚、并行编辑拒绝、重复回滚与清理。真实无线驱动、DFS、设备断电及漫游行为仍需在有线维护窗口另行验证。
