# wifiroam · OpenWrt 无缝漫游组网

一套 **OpenWrt 原生的多 AP 无缝漫游（802.11k/v/r）管理方案**，由两个可独立安装的软件包组成：

| 软件包 | 作用 | 架构 |
|---|---|---|
| `wifiroam` | ucode 守护进程 `wifiroamd` + NFQUEUE DPI 二进制 `wifiroam-dpi` | 目标架构（如 `aarch64_cortex-a53`） |
| `luci-app-wifiroam` | LuCI Web 管理界面（深色卡片式 UI，四主题拓扑） | `all`（架构无关） |

守护进程以 CAP（主路由）/ SAP（子 AP）角色运行：发现邻近设备、人工配对、聚合全网拓扑、跟踪终端，并从主路由统一下发漫游、访客网络、回程与应用管控配置。管理通过 ubus 对象 `wifiroam`（23 个方法）完成，LuCI 插件提供网络拓扑、客户端、无线设置和高级设置四个页面。

本仓库构建并发布软件包成品，不含可刷固件镜像。

## 能做什么

- **网络拓扑**：自动绘制主路由 / 子 AP / 终端的连接关系，区分有线回程与无线链路，按信号强度着色；支持终端重命名、固定 AP、详情统计。
- **无缝漫游**：集中配置 802.11k（邻居/信标）、802.11v（BTM）、802.11r（FT），可选简单/专家 FT 模式与 RSSI 阈值；下发前做 wpad/加密分组预检。
- **无线与访客网络**：多频合一或分频 SSID/密码统一下发；访客网络支持限速、允许时段、隔离与开放网络。
- **应用识别与管控**：基于 DPI（TLS/QUIC SNI）+ nftset 的按应用时长统计与允许/阻断策略。
- **终端管理**：29,000+ OUI 厂商识别、主机名→厂商推断、随机 MAC 识别、设备分组与拆分、终端历史（首次/最后在线）、定时断网（ban，按星期与时段）、固定 AP（pin）、WoL 唤醒。
- **拓扑可视化**：四套主题（经典/地图/深色/极简，可在页面切换）、101 张真实机型照片（无匹配时回退矢量造型）、按信号强度着色的链路、富设备卡片。
- **硬件卸载**：查看与切换 firewall flow offloading。

> 具体能力以真机为准。DPI/访客限速/硬件卸载依赖内核 NFQUEUE、nftables、`tc` 等能力，极简镜像下会降级。

## 获取 IPK / APK

推送提交会触发 [GitHub Actions 构建](../../actions/workflows/build.yml)，也可在该页点 **Run workflow**。从成功运行的 **Artifacts** 下载对应 ZIP；每份都含 `wifiroam` 与 `luci-app-wifiroam` 两个包。

| 固件与软件包架构 | 格式 | Actions 制品 |
|---|---|---|
| OpenWrt 24.10.5 x86/64，`x86_64` | IPK | `openwrt-24.10.5-x86-64-ipk` |
| OpenWrt 25.12.6 mediatek/filogic，ARM64 Cortex-A53，`aarch64_cortex-a53` | APK | `openwrt-25.12.6-aarch64-cortex-a53-apk` |
| OpenWrt 24.10.5 mediatek/filogic，ARM64 Cortex-A53，`aarch64_cortex-a53` | IPK | `openwrt-24.10.5-aarch64-cortex-a53-ipk` |

先用 `cat /etc/openwrt_release` 核对固件版本，再用 `apk --print-arch` 或 `opkg print-architecture` 核对软件包架构。**CPU 名称相同不代表 ABI 匹配**；不同版本、目标或 ABI 请勿强制安装。

在 Linux x86_64 构建机上可复现（普通用户）：

```sh
SDK_OUTPUT_DIR="$PWD/dist/ipk" bash scripts/build-sdk.sh x86-64-ipk
SDK_OUTPUT_DIR="$PWD/dist/apk" bash scripts/build-sdk.sh aarch64-cortex-a53-apk
```

构建脚本校验官方 SDK 的 SHA-256，并固定官方 base/packages/luci feed 的提交；先编译 `libmnl` / `libnfnetlink` / `libnetfilter-queue` 到 staging_dir，再编译两个软件包。CI 分别编译并上传对应产物。

## 安装

```sh
opkg install wifiroam_*.ipk luci-app-wifiroam_*.ipk                    # IPK
# 或
apk add --allow-untrusted wifiroam-*.apk luci-app-wifiroam-*.apk       # APK
```

安装后访问 LuCI 的 **网络 → Network Management**。首次进入为 `unconfigured` 角色：

1. 在「无线设置」把本机设为 **主路由（CAP）**，设置配对口令。
2. 在其他 AP 上设为子路由，或在主路由「发现的设备」里按 MAC 配对。
3. 配对完成后到「网络拓扑」确认节点在线，再到「高级设置」配置漫游参数。

默认配置不含任何口令或密钥。守护进程在启用前不会下发配置；无线相关写入会重载网络并可能短暂中断 Wi-Fi，请保留有线维护通道。

## 依赖

`wifiroam` 运行时依赖官方 feed 中的：`ucode` 及 `ucode-mod-{ubus,uloop,socket,fs,uci,digest}`、`libnetfilter-queue`、`libmnl`、`libnfnetlink`。部分功能还需要系统具备 `nft`、`ip`、`iw`、`tc`、完整版 `wpad`（非 basic）、dnsmasq 与内核 NFQUEUE 支持。

## 仓库结构

```
package/wifiroam/           ucode 守护进程 + DPI 源码 + init/config
luci-app-wifiroam/          LuCI 前端（4 页面 + 深色主题 CSS + zh_Hans 翻译）
scripts/build-sdk.sh        固定版本的 SDK 交叉构建与产物校验
tests/test_contract.py      后端/前端/ACL/菜单一致性离线校验
.github/workflows/build.yml 静态检查 + 三目标构建矩阵
```

## 许可证

GPL-2.0-or-later，见 [LICENSE](LICENSE)。
