# meshd 0.1 控制面契约

控制面使用 IPv4。客户端数据不经过 `meshd`，由 OpenWrt 管理的现有桥和回程承载。`net_name` 是网络标识，不是密码；整个集群精确匹配（大小写敏感）。

## LuCI / ubus

对象名 `meshd`。成功为 `UBUS_STATUS_OK`；未获角色授权为 `PERMISSION_DENIED`，非法参数为 `INVALID_ARGUMENT`，找不到节点为 `NOT_FOUND`，超过 10 个授权节点为 `NO_MEMORY`，持久化失败或状态损坏为 `SYSTEM_ERROR`。暂不可执行（如有事务在途，或已确认后要求回滚）的操作返回 `NOT_SUPPORTED`。角色限制由守护进程执行；LuCI 写方法还受 rpcd ACL 约束。

| 方法 | 权限 | 参数 | 结果和边界 |
|---|---|---|---|
| `status` | 读 | 无 | `enabled, role, node_id, transport, gateway_connected, error, backhaul, applied_version`；`transport` 是服务初始化状态，不等于网关在线 |
| `topology` | 读 | 无 | `nodes:[{id,role,ip,model,state,approved,version,config_failed}]`；表示网关控制面可见的节点，不代表物理父子拓扑 |
| `link_quality` | 读 | 无 | `{available:false}`；无实测时不编造评分 |
| `client_list` | 读 | 无 | `{available:false,clients:[]}` |
| `roam_config` | 读 | 无 | `available, ssid, k_neighbor, k_beacon, v_bss_transition, ft, version`；不返回密钥 |
| `config_status` | 读 | 无 | `version, target_version, state, error, rollout, approved_nodes, converged_nodes, drift` |
| `diagnose` | 读 | 无 | `checks:[{name,status,detail}]`，`status` 为 `PASS/WARN/FAIL`；不返回证书、私钥和密码 |
| `upgrade_status` | 读 | 无 | `{available:false}` |
| `event_log` | 读 | 无 | `events:[{type,time,detail}]`，`time` 为 Unix 秒；最多 128 条，只在 RAM，从新到旧 |
| `enroll` | 网关写 | `{node_id:string}` | 指纹只接受 64 个小写十六进制字符。人工按离线指纹授权；重复同指纹幂等，不下发证书 |
| `remove_node` | 网关写 | `{node_id:string}` | 持久撤销控制面授权；不会从对方设备抹掉已有配置 |
| `config_apply` | 网关写 | `{ssid:string,key:string,k_neighbor:bool,k_beacon:bool,v_bss_transition:bool,ft:bool}` | `{accepted:true,target_version}` 仅表示已受理；轮询 `config_status`、`topology` 确认本地及节点结果 |
| `config_rollback` | 网关写 | 无 | 取消本机尚未确认的事务；需要等事务状态返回 `rolled_back`。不能撤回远端已确认的配置 |
| `set_role`、`set_roam`、`set_backhaul`、`set_guest`、`set_qos`、`set_parental`、`upgrade_node`、`optimize_topology` | 预留，无 LuCI 授权 | 暂无 | 固定 `NOT_SUPPORTED`；未来定义各自字段后才启用 |

本地阶段：`idle → applying → verifying → committed`。错误或取消可进入 `rolling_back → rolled_back`；无法安全恢复是 `failed`，此时保留事务材料以便人工处理。`version` 是上一次**本机**确认的版本，`target_version` 是最近受理的尝试。网关 `rollout` 为 `idle / waiting / paused / complete`；`complete` 表示当前检查中所有已授权节点的有效心跳版本和签名帧摘要一致，不表示客户端已实连或无线漫游已实测。

## 线上帧（协议版本 1）

整数为网络字节序；字段以字节计。内网防火墙应仅在管理接口放行。应用负载严格按长度解析，不经过 JSON 或 shell。

| 通道 | 格式 | 信任或回执 |
|---|---|---|
| UDP `41007` | `MSH\x01` 版本族标识、1 B 角色、16 B `SHA-256(net_name)` 前缀、32 B 叶证书指纹、1 B 型号长度、ASCII 型号 | 发现提示，不认证；网关采信 socket 源地址用于展示。同网名称过滤不是加密，也不是认证。没有凭据或载荷地址 |
| TCP `41008` | 每连接 1 个请求和最多 1 个应答，两字节大端长度 + 1–2048 字节载荷 | 管理地址/接口绑定、TLS 1.2+、CA 双向校验；节点固定网关叶指纹，网关按授权指纹检查；不使用 TLS 0-RTT 或会话恢复 |
| `MHB\x01` | 4 B magic + version `u32` + rejected_version `u32` + drift `u8` + model_len `u8` + 32 B 完整网络哈希 + 32 B 已确认签名帧哈希 + 型号 | 约每 3 秒短连接一次；网关以 TLS 认证叶的实际指纹关联记录，不信请求方自报身份；没有配置时两种版本/摘要字段可为 0 |
| `MAK\x01` | 4 B magic | 无新配置时的认证心跳回执 |
| `MCF\x01` | 4 B magic + Version `u32` + feature_flags `u8`（低四位依次 k 邻居/k 信标/v BTM/r FT）+ ssid_len `u8` + key_len `u8` + 32 B 完整网络哈希 + SSID + 密钥 + signature_len `u16` + signature | 网关叶密钥签 SHA‑256，节点只用该已认证网关叶公钥验签。仅接收新于本机已确认及已拒绝版本的配置。节点下一轮上报已确认版本和签名帧摘要或拒绝版本 |

网关在线状态在约 9 秒未收到有效心跳后为 `suspect`、约 15 秒后为 `offline`。连接每次重建，单个连接约 2.5 秒截止。候选仅做约每 5 秒的发现广播并以约 20 秒过期。发现广播可伪造，必须人工核对指纹；短时间去重不是认证防重放。控制消息的新鲜性来自一次一连接的 TLS 和严格单调配置版本，不额外引入共享 HMAC 密钥或应用时间戳。

手册设想的线上 `enroll/enroll_ack`（令牌、自动发证）、`topology_*`、`path_switch_notify`、`config_rollback/config_query`（远端命令）、`client_report/event_report`、`diagnose_cmd/diagnose_result`、`upgrade_*` 均未开放。不要为兼容预留协议放行未认证的操作。

## 配置一致性

`core.c` 持有事件环、有界候选/节点表和本地事务。`transport.c` 通过 `mesh_transport_ops` 回调，不访问 UCI；`wifi.c` 只碰显式纳管 `wireless` 的 WPA2 AP 字段；`host.c` 在限时子进程中访问 netifd/hostapd，不阻塞 TLS 事件循环；`rpc.c` 提供只读数据和受角色限制的写入口。

先生成候选无线文件和 0600 原始备份，再持久写事务标记；这样崩溃重启时仍能决定恢复的方向。候选以同目录 `rename` 公开，校验本机 AP 与认证管理心跳后，把经签名的配置记录原子落盘。恢复时若当前无线文件既不等于备份也不等于本次候选，守护进程宁可暂停，也不会覆盖管理员改动。UCI 和其他编辑器不共享覆盖整个 `rename` 时序的锁，因此仍须在有线维护窗口内避免并行编辑。

网关先确认自己的配置，然后一次只下发给一个尚未收敛且已在线的节点，确认成功后约 5 秒才尝试下一个；未在约 45 秒收到目标节点确认则暂停。故障节点有本机自动恢复机制；这不是全网原子提交、也不提供整批回滚。恢复或刷新授权必须由管理员决定。该版本不防具有 root 权限的恶意设备本地回滚状态文件；切换根 CA、设备身份或集群名需先备份并重建信任和版本状态，不能直接覆盖当前生效环境。
