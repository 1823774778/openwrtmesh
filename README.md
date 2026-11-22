# openwrtmesh · meshd 0.1

基于《OpenWrt 全屋 Mesh 组网方案》和《meshd 全量提示词手册》的 **V1 试用版**。这是两个可以独立安装或集成到固件的 OpenWrt 软件包：C 守护进程 `meshd` 和 LuCI 插件 `luci-app-meshd`。插件默认关闭控制面，先部署现有回程，再启用组网管理。

## 能做什么

- 在指定 IPv4 管理接口发现候选设备。离线生成每台设备独立的证书，管理员按已核对的指纹授权；已授权节点用 mTLS 心跳连网关。LuCI 显示在线、可疑、离线和配置版本。
- 从网关统一管理选定的 WPA2-Personal 客户端 AP 的 SSID、密码和 802.11k/v/r 开关。签名配置先在网关确认，再逐台下发给节点；每台设备做本地健康校验和失败回滚。密码不由 LuCI/RPC 回显。
- 使用手工建好的有线桥接或可用的单跳 WDS 回程，提供诊断和事件记录。界面位于 **网络 → Mesh**。策略和升级页只显示当前不可用，不提供无效操作。

**当前不是自动选路的多跳 Mesh 固件。** 不自动建网桥/VLAN、不自动切换回程；未实现 802.11s/batman-adv 自愈、主动漫游引导、整批回滚、访客/IoT/QoS 或 OTA。兼容性、漫游时延、路径恢复与资源指标待真机验收。分项范围见 [验收矩阵](<docs/validation.md>)。

## 获取 IPK / APK

推送提交会触发 [GitHub Actions 构建](https://github.com/1823774778/openwrtmesh/actions/workflows/build.yml)，也可在该页点 **Run workflow**。从成功运行的 **Artifacts** 下载对应 ZIP；每份都含 `meshd` 和 `luci-app-meshd` 两个包。

| 固件与软件包架构 | 格式 | Actions 制品 |
|---|---|---|
| [OpenWrt 24.10.5 x86/64](https://downloads.openwrt.org/releases/24.10.5/targets/x86/64/)，`x86_64` | IPK | `openwrt-24.10.5-x86-64-ipk` |
| [OpenWrt 24.10.5 mediatek/filogic](https://downloads.openwrt.org/releases/24.10.5/targets/mediatek/filogic/)，`aarch64_cortex-a53` | IPK | `openwrt-24.10.5-aarch64-cortex-a53-ipk` |
| [OpenWrt 25.12.6 mediatek/filogic](https://downloads.openwrt.org/releases/25.12.6/targets/mediatek/filogic/)，`aarch64_cortex-a53` | APK | `openwrt-25.12.6-aarch64-cortex-a53-apk` |

先在路由器用 `cat /etc/openwrt_release` 核对固件版本，再用 `apk --print-arch` 或 `opkg print-architecture` 核对软件包架构。**CPU 名称相同不代表软件包 ABI 匹配**；不同版本、目标或 ABI 请使用对应 SDK，不要强制安装。

包还绑定固件所用的库 ABI 名。安装报 `no such package` 时，先在设备上跑只读诊断，把输出贴到 Issue：

```sh
sh scripts/check-device.sh
```

| 制品 | 设备上必须已存在的库 |
|---|---|
| 24.10.5 IPK（x86_64 / `aarch64_cortex-a53`） | `libubox20240329`、`libubus20250102`、`libblobmsg-json20240329`、`libuci20250120`、`libmbedtls21` |
| 25.12.6 APK（`aarch64_cortex-a53`） | `libubox20260213`、`libubus20251202`、`libblobmsg-json20260213`、`libuci20250120`、`libmbedtls21` |

构建脚本会打印实际生成的依赖清单，以它为准。设备上的库名不同就说明固件版本不匹配：用该版本的 SDK 重编，或升级到对应官方固件；`--force-*` 只会让动态链接在启动时失败。

在自有 Linux x86_64 构建机上也可复现（普通用户）：

```sh
# Debian / Ubuntu 的 SDK 宿主依赖
sudo apt-get install build-essential binutils curl file gawk gettext git libncurses-dev \
    patch perl pkgconf python3 rsync unzip wget zlib1g-dev zstd
SDK_OUTPUT_DIR="$PWD/dist/ipk" bash scripts/build-sdk.sh x86-64-ipk
SDK_OUTPUT_DIR="$PWD/dist/apk" bash scripts/build-sdk.sh aarch64-cortex-a53-apk
```

两种目标都在 Linux x86_64 构建机上交叉构建，无需 ARM 主机。[构建脚本](<scripts/build-sdk.sh>) 校验官方 SDK SHA-256，并固定官方 feeds 的提交；[CI](<.github/workflows/build.yml>) 分别编译和上传对应的两个包，不输出可刷固件。

其他目标的通用集成方式：在**相同固件版本与目标**的官方 SDK/源码树中，把 `package/meshd` 和 `luci-app-meshd` 两个包目录分别放到 `package/meshd`、`package/luci-app-meshd`。按目标版本配置官方 feeds，安装 `libubox`、`libubus`、`libuci`、`libblobmsg-json`、`libmbedtls`、`luci-base` 对应配方；选择 `CONFIG_PACKAGE_meshd=m` 和 `CONFIG_PACKAGE_luci-app-meshd=m` 后执行：

```sh
make defconfig
make -j"$(nproc)" package/meshd/compile
make -j"$(nproc)" package/luci-app-meshd/compile
```

若集成到完整固件，改用 `=y`，再按具体设备选择完整 wpad（如 `wpad-mbedtls` 或 `wpad-openssl`）与 HTTPS LuCI。插件不需要 Node.js/Python 在设备上运行。`ip-full`、`tc`、`nftables`、`batman-adv` 按固件的实际网络方案选择，并非本版核心依赖。不要在镜像内预置生产 CA 私钥、节点私钥或 Wi‑Fi 密码。

## 安装和使用

按 [部署手册](<docs/deploy.md>) 配好管理网络和客户端 AP、匹配软件包、生成设备证书、核对指纹并启用。建议从一台网关和一台有线节点开始，全程保留有线维护。无线发布会重载 AP；不要在只有 Wi‑Fi 的维护会话里首次试用。

- [协议和 ubus API](<docs/protocol.md>)：参数、返回值、线上帧与事务边界。
- [验收矩阵](<docs/validation.md>)：自动测试覆盖、真机用例、未实现项目。
- [许可证](<LICENSE>)：GPL-2.0-or-later。
