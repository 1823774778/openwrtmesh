# OpenWrt 24.10 / 25.12 部署手册

> 先在 **一台网关 + 一台节点** 的隔离测试网中试用，并保留有线维护口。发布会重载无线，已有客户端会短暂断开，修改 SSID 或密码后客户端需重新连接。默认关闭 Mesh 控制网络；安装不会替你迁移 WAN、DHCP、VLAN 或无线回程。

## 1. 准备回程和 AP

- **首选有线**：网关负责 WAN、NAT、DNS 和 DHCP。先把节点按现有 OpenWrt [桥接 AP](https://openwrt.org/docs/guide-user/network/wifi/wifiextenders/bridgedap) 方式接入，给节点配置不冲突的管理 IP；业务网只运行**一台** DHCP 服务器。不要用两个并行的桥接路径测试。
- **单跳 WDS（可选）**：按 [OpenWrt WDS 指南](https://openwrt.org/docs/guide-user/network/wifi/wifiextenders/wds) 手工配置并验证驱动支持；回程 BSS/STA **不得放进本插件的 AP 列表**。优先用独立 radio，不能带着另一路并行有线桥接盲试。桥上是否能透传管理 VLAN，由设备和驱动决定，先独立验证。首轮验收用有线。
- 为控制面配置**独立管理 VLAN/网段**和静态 IPv4。示例：网关 `192.168.90.1/24`、节点 `192.168.90.2/24`、定向广播 `192.168.90.255`。地址只是示例，不要求 VLAN ID 4094；建立 VLAN 需按硬件 DSA/交换机配置。仅在该受控接口/防火墙区放行 UDP `41007`（同二层的发现）和到网关的 TCP `41008`，不要对 WAN、IoT、访客或普通 Wi‑Fi 客户端开放；不要为此关闭防火墙。检查节点能通过管理地址访问网关，确认时钟已同步。
- 用 `opkg list-installed`（24.10）或 `apk list --installed 'wpad*' 'hostapd*'`（25.12）确认安装了**完整 wpad**（如 `wpad-mbedtls` 或 `wpad-openssl`），不能是 `wpad-basic-*`。预检也支持完整 hostapd 包。请按当前固件的包源与官方说明，用有线连接备份后替换冲突包；不要在生产 Wi‑Fi 上盲目卸载。仅管理 mac80211 的 WPA2-Personal `psk2` / `psk2+ccmp` 客户端 AP，不管理 6 GHz、WPA3、WDS、企业认证、自定义 PSK 文件或 VLAN 型 AP。若某客户端不兼容 11r，先保持 FT 关闭。
- 先通过 LuCI 正常启用需要纳管的客户端 AP，使之关联到已配置好的业务 LAN。为每个 AP 在本机的 `/etc/config/wireless` 中使用稳定的**命名 section**和唯一明确的 `option ifname`。例如在原有 `radio0` 上编辑一个测试 AP（`lan` 必须是你既有的业务网络；设好真实测试密钥，勿复制示例字面密码）：

```uci
config wifi-iface 'mesh_clients_2g'
    option device 'radio0'
    option ifname 'mesh-ap0'
    option mode 'ap'
    option network 'lan'
    option encryption 'psk2'
    option ssid 'InitialTestAP'
    option key 'REPLACE_WITH_TEST_PSK'
```

用 `ubus call hostapd.mesh-ap0 get_status` 确认返回 `status: ENABLED`。显式 `ifname` 不能与设备现有接口或自动生成的 Wi-Fi 接口名称重名。在 5 GHz 测试前确认信道和监管域；需要超过 30 秒 DFS CAC 的信道可能触发回滚。不要用匿名 AP 的动态序号纳管，也不要把桥接、访客或回程 AP 加入 `list ap`。插件不改 WDS 配置，但无线重载仍可能使共用 radio 的 WDS 短暂掉线。同一设备可以纳管最多 8 个预先选定的 AP；一个网关最多授权 10 个节点（软件容量，不是压力测试结果）。

## 2. 安装正确架构的软件包

用 `cat /etc/openwrt_release` 核对 **OpenWrt 版本和目标/子目标**；用 `opkg print-architecture` 或 `apk --print-arch` 核对软件包架构。仅从[本项目的 GitHub Actions](https://github.com/1823774778/openwrtmesh/actions/workflows/build.yml) 下载匹配的制品，通过有线维护网把解压后的两个包传到 `/tmp`。如果设备版本、目标或 ABI 不匹配，请用设备对应的 SDK 重编，不要绕过依赖或架构检查。

**OpenWrt 25.12.6 / mediatek/filogic / `aarch64_cortex-a53`（APK）：**

```sh
apk update
apk add --allow-untrusted /tmp/meshd-0.1.0-r2.apk /tmp/luci-app-meshd-0.1.0-r1.apk
```

Actions 的两个 APK 是未签名的独立构建包；仅在核对下载来源和摘要后，对这条安装命令临时使用 `--allow-untrusted`。不要关闭软件源的签名验证；生产环境建议使用自己的签名包源。如果使用后续版本，把命令里的文件名换成实际下载的文件名。

**OpenWrt 24.10.5 / x86/64 / `x86_64`（IPK）：**

```sh
opkg update
opkg install /tmp/meshd_0.1.0-r2_x86_64.ipk /tmp/luci-app-meshd_0.1.0-r1_all.ipk
```

两种格式安装后均可运行：

```sh
/etc/init.d/meshd enable
/etc/init.d/meshd start
ubus call meshd status
```

如菜单未刷新，重新登录 LuCI；使用 HTTPS。`core.enabled=0` 时守护进程只提供本地 `ubus` 状态，不打开控制面端口。现有 LuCI 账号若不是管理员，rpcd 只需给 `luci-app-meshd` 对应的 read 或 read/write 授权；不要把全局写授权当作打开菜单的必要条件。

## 3. 离线制作证书

在受信任的离线 Linux 机器上运行 [PKI 工具](<../scripts/pki.sh>)，每个设备生成**不同**的 P‑256 密钥。生产 CA 私钥不出离线环境，不上传到 GitHub，不拷到路由器；脚本不为 CA 私钥添加口令，请把 CA 目录放在受保护的磁盘上。

```sh
mkdir -m 700 pki
sh scripts/pki.sh init pki/ca
sh scripts/pki.sh issue pki/ca gateway gw pki/gw
sh scripts/pki.sh issue pki/ca node ap2 pki/ap2
sh scripts/pki.sh fingerprint pki/gw/node.crt
sh scripts/pki.sh fingerprint pki/ap2/node.crt
```

把各自目录下三份文件 `ca.crt`、`node.crt`、`node.key` 通过可信维护通道放入对应设备的 `/etc/meshd/identity/`。目录为 root 所有、0700；文件设为 root 所有、0600、常规文件（非软链接）。不要把同一份私钥复制到两台路由。网关与节点应使用同一私有 CA，节点另需**通过可信渠道独立核对的网关叶证书指纹**。系统时钟错误、证书过期、用错设备的私钥或角色证书都会拒绝建立会话。`node.crt` 虽然命名相同，但网关的用途为 `serverAuth`，节点为 `clientAuth`。工具生成的叶证书小于控制面存储的 2048 字节 DER 上限。

## 4. 配置网关和节点

在本机的 `/etc/config/meshd` 设置管理参数；这是系统管理员的本地操作，LuCI 插件不改角色或设备的 VLAN。下面演示网关，AP section 需要改为你的实际名称；保留默认 `mtls=1`、`allow_wps=0`。

```sh
uci set meshd.core.role=gateway
uci set meshd.core.net_name=HomeMeshLab
uci set meshd.core.bind_ip=192.168.90.1
uci set meshd.core.broadcast_ip=192.168.90.255
uci set meshd.core.model=Gateway
uci set meshd.backhaul.protocol=wired
uci add_list meshd.roam.ap=mesh_clients_2g
uci set meshd.core.enabled=1
uci commit meshd
/etc/init.d/meshd restart
ubus call meshd status
```

节点起步可设为 `candidate`，只宣布候选、不发 TLS；使用对应节点证书。节点实际角色为 `node` 时需要网关 IP 和真实指纹。

```sh
uci set meshd.core.role=candidate
uci set meshd.core.net_name=HomeMeshLab
uci set meshd.core.bind_ip=192.168.90.2
uci set meshd.core.broadcast_ip=192.168.90.255
uci set meshd.core.model=AP2
uci set meshd.backhaul.protocol=wired
uci add_list meshd.roam.ap=mesh_clients_2g
uci set meshd.core.enabled=1
uci commit meshd
/etc/init.d/meshd restart
```

在网关打开 **网络 → Mesh → Nodes**，查看候选或手动填入已核对的节点叶证书指纹。**只按离线指纹授权，不能信任候选名字、地址或广播里的指纹本身。** 然后在节点运行以下命令，把 `GATEWAY_SHA256_64_HEX` 换成已核对的 64 位小写十六进制值。管理地址不变时，只要重新保存、重启。也可直接部署 `node`，由网关手输指纹，不必先做 `candidate` 阶段。

```sh
uci set meshd.core.role=node
uci set meshd.core.gateway_ip=192.168.90.1
uci set meshd.core.gateway_id=GATEWAY_SHA256_64_HEX
uci commit meshd
/etc/init.d/meshd restart
ubus call meshd status
```

网关 `ubus call meshd topology` 上应开始显示认证在线，而节点本地 `status.gateway_connected` 变为真。ARP/DHCP、物理网线、VLAN 和无线驱动正常与否不以这两个 RPC 单独判定。

## 5. 发布和回退

1. 先为路由配置做离线备份，**保持有线维护**，等待其他 LuCI Apply/确认窗口结束。发布期间不要另用 LuCI/uci 编辑无线配置或同时升级系统。
2. 网关页面 **Roaming and Wireless** 输入目标 SSID（UTF-8 1–32 字节）和 WPA2 密码（8–63 可打印 ASCII 字节或 64 位十六进制），选择 k/v 和可选 FT。提交仅表示已受理，界面不回显密码。先看网关获得本地 `committed`；然后看授权节点 `version`、`config_failed` 与 `rollout`，最后用终端实连两个 AP 验证。
3. 校验期间 AP 需要报告新 SSID 且处于 `ENABLED`；节点还需要新一轮经认证的心跳。超过约 30 秒仍未健康会恢复本机旧配置。网关可在本机未提交时取消；确认后的网关不能一键回退全网。如要回退已提交的配置，用之前保留的 SSID/密码重新发布**更高版本**。若节点拒绝、漂移或超时，网关暂停后续下发，先修复，随后重新发布；已提交的其他设备不自动回退。掉线但未触发暂停的节点恢复后可继续同步。
4. 如果恢复因外部修改或存储故障而被阻止，保留 `/etc/meshd/transaction` 和 0600 的 `wireless.backup` / `wireless.stage`。用有线登录，停掉 `meshd`，离线备份三份和当前无线文件，再由管理员比较备份与现状后决定保留哪版。确认正确的 `/etc/config/wireless` 并手动重载后，才处置该事务的残留文件；不要把备份、密钥或签名配置原文提交到 Issue。
5. **停用或卸载**：先令 `meshd.core.enabled=0`、提交并重启确认没有在进行的事务；若要停止服务，再执行 `/etc/init.d/meshd stop` 和 `/etc/init.d/meshd disable`。停用不会擦掉证书，也不会撤销已经确认的无线配置。卸载时先卸 `luci-app-meshd` 再卸 `meshd`；只在确认不再需要原身份与配置后手工清理私有状态。节点授权删除只撤销其控制面访问，不会让它忘记此前的 Wi‑Fi 密码；设备丢失时还要更换 SSID 密码并从管理网络移除该设备。

## 快速排查

| 现象 | 优先检查 |
|---|---|
| 网关显示控制面未启动 | `ubus call meshd status` 和 `logread -e meshd`；管理地址是否真的在本机接口、广播是否匹配该子网、文件属主与权限、是否已启用 |
| 无候选 | 同一二层管理 VLAN、候选 `role=candidate`、UDP 防火墙、两边 `net_name`；可以人工核对并直接填指纹 |
| 授权过仍离线 | 节点是否切换 `node`、网关 pin、TCP 41008、CA/叶有效期和 EKU、两端时钟；看管理网络而不是业务网访问 |
| 提交失败或回退 | `ubus call meshd diagnose`、`config_status`、`event_log` 和 `hostapd.<ifname> get_status`；检查 wpad、所选 AP、未提交 LuCI 编辑、DFS、是否误选回程 AP |
| 一部分节点不更新 | `rollout` 是否暂停，比较各版本、失败标记、已知无线配置漂移；通过有线逐台修复，不要删除版本文件逃过校验 |
| 版本对但客户端不漫游 | 客户端决定是否漫游；检查 SSID、加密/密码、频道覆盖、r 兼容性并重做实际终端连接测试 |
