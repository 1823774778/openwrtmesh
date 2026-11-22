# Wi-Fi 事务测试（OpenWrt 24.10 / 25.12.6）

[测试](<package/meshd/tests/test_wifi.c>) 使用真实的 libuci 与 Mbed TLS，但只访问临时测试目录。不运行 APK/opkg、hostapd 或 netifd，不发 ubus，也不操作网卡。测试由 GitHub Actions 的 x86/64 runner 执行，不应在生产路由器上运行。`sudo` 用于验证配置及包元数据必须归 root 所有的规则。

[隔离测试脚本](<package/meshd/tests/run-wifi.sh>) 接受已解包、已构建依赖的 OpenWrt 24.10.5 x86/64 SDK 路径：

```sh
SDK=/path/to/x86-64-sdk bash package/meshd/tests/run-wifi.sh
```

也可在已安装 libuci、libubox 和 Mbed TLS 3.x 开发库的隔离 Linux 测试机上省略 `SDK`，使用该 runner 的 C 编译器。ARM64 APK 交叉构建和 x86/64 上的元数据与事务测试是两项独立的验证。

预期输出 `wifi transaction smoke passed`。覆盖 24.10 opkg、25.12 APK 完整 wpad/mesh/hostapd、APK 与陈旧 opkg 并存、卸载、basic/冲突、非法列表/权限/链接/可执行文件，以及 AP 安全约束、CLI/LuCI 暂存和覆盖文件、PSK/SSID 转义与漂移检测、FT 参数、WPS 关闭、保留非目标 WDS/网络/PMF 设置、持久备份及字节级回滚。真实无线驱动、DFS、设备断电及漫游行为仍需在有线维护窗口另行验证。

## APK 预检依据

- [25.12.6 打包规则](https://github.com/openwrt/openwrt/blob/v25.12.6/include/package-pack.mk) 将普通文件或链接的绝对路径按行写入 `/lib/apk/packages/${pkgname}.list`。列表本身也是包拥有的文件。[base-files 的安装/卸载脚本](https://github.com/openwrt/openwrt/blob/v25.12.6/package/base-files/files/lib/functions.sh) 使用这份列表；[APK 的卸载实现](https://github.com/alpinelinux/apk-tools/blob/b5a31c0d865342ad80be10d68f1bb3d3ad9b0866/src/database.c) 随包内容一同清理它。
- [官方包定义](https://github.com/openwrt/openwrt/blob/v25.12.6/package/network/services/hostapd/Makefile) 和 [Cortex-A53 base 发布目录](https://downloads.openwrt.org/releases/25.12.6/packages/aarch64_cortex-a53/base/) 中的 hostapd/wpad 包名不含 ABI 后缀（APK 文件名另外包含版本）。完整 wpad 的列表包含 `/usr/sbin/wpad` 与 `/usr/sbin/hostapd`，`hostapd` 是指向 `wpad` 的相对链接；完整版 hostapd 则安装独立可执行文件。`hostapd-common` 和 `hostapd-utils` 不算提供者。
- APK 列表只读检查。仅信任 root 拥有且组/其他用户不可写的普通文件与受控目录；拒绝列表损坏、符号链接或冲突变体，不启动 APK CLI、不解析 APK 数据库。APK 状态优先，失败也不采用陈旧 opkg 状态；24.10 保留原 opkg 预检。[官方 filogic 设备清单](https://downloads.openwrt.org/releases/25.12.6/targets/mediatek/filogic/profiles.json) 默认使用 `wpad-basic-mbedtls`，仍须换成受支持的完整变体（如 `wpad-mbedtls`）。
