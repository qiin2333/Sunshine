# Windows DualSense 组件生命周期实施方案

## 1. 文档状态

- 状态：实施设计稿
- 目标平台：Windows 10/11 x64（实验性）；首期本机验证基线为 Windows 11 24H2 build 26100
- GUI：Sunshine Control Panel（Tauri + Vue + Element Plus）
- 运行时：Sunshine Core + 独立 DualSense Sidecar
- 初期外部组件：HIDMaestro 运行时；普通 HID 使用其 UMDF2 后端，四声道复合设备按需使用其 USB/IP 后端
- 设计原则：沿用 Web 串流组件的安装体验，但由 Sunshine Core 持有串流运行时生命周期

本文定义组件安装、系统传输、虚拟设备、串流会话和 GUI 的边界。它不把 HIDMaestro 内部代码复制进 Sunshine，也不改变现有 ViGEm 支持范围。

## 2. 目标与非目标

### 2.1 目标

1. 用户可以在 GUI 内完成 DualSense 组件的检查、下载、校验、安装、修复、测试、更新和卸载。
2. Sunshine 在需要 DS5 的串流会话开始前创建虚拟设备，在最后一个会话结束后可靠释放。
3. 保留 ViGEm 对 X360、DS4 等成熟设备的现有路径；DS5 使用独立、可选的运行时。
4. 对 HID、四声道音频端点和 HD Haptics 数据通路分别显示可诊断状态。
5. GUI、Sunshine Core 或 Sidecar 异常退出时，不残留无法管理的虚拟设备。
6. 外部组件的来源、版本、摘要和许可证对用户透明。

### 2.2 非目标

- 第一阶段不把 HIDMaestro 或 USB/IP 实现静态链接进 Sunshine。
- 第一阶段不随 Sunshine 安装包捆绑当前 HIDMaestro 发布包。
- 不自动卸载系统级 USB/IP 传输驱动。
- 不用 DS5 路径替换 ViGEm。
- 不在没有 DS5 串流或用户测试时长期创建虚拟手柄。

## 3. 核心决策

### 3.1 已验证的 HIDMaestro 能力边界

以 HIDMaestro v1.6.2 官方发布物和同标签源码为准，Sunshine 必须区分两类 profile，不把 USB/IP 当作所有 DS5 模拟的必需条件：

| Profile | 后端 | 虚拟 HID | 四声道扬声器/触觉 | 系统级依赖 | 第一阶段用途 |
|---|---|---:|---:|---|---|
| `dualsense` | UMDF2 | 是 | 否 | HIDMaestro 动态生成并安装的 UMDF2 驱动/本机自签名证书 | 输入、触摸板、运动、自适应扳机验证与无 HD Haptics 的降级模式 |
| `dualsense-composite` | USB/IP | 是 | 是 | 内嵌 usbip-win2 0.9.7.7 | 需要游戏识别 DS5 四声道端点并产出 authored haptics 的完整模式 |
| `dualsense-composite-genshin` | USB/IP | 是 | 是 | 与完整模式相同 | 实验性《原神》兼容身份；只把 USB product string 改为首发版 `Wireless Controller` |

复合 profile 的音频输出固定为 48 kHz、16-bit、4 声道，角色依次为 `speakerLeft`、`speakerRight`、`hapticLeft`、`hapticRight`。HIDMaestro 公共 API 可以直接交付游戏写入该端点的 PCM 帧，Sidecar 不需要从混合后的桌面音频重新猜测第 3/4 声道。

兼容 profile 不维护第二份 USB 描述符：Sidecar 启动时从已校验的 `dualsense-composite` 动态派生，只修改 profile ID、显示名和 product string。GUI 默认关闭该模式，并只在 DualSense、HD Haptics、USB/IP 与新 Sidecar 能力均可用时允许启用。切换后需要重新创建虚拟手柄，用户应先开始串流，再完全退出并重新启动《原神》。该模式不修改 Windows 默认播放或录音设备，现有 never-default 防线保持不变。

官方发布物当前未做 Authenticode 签名，并携带运行时、usbip-win2 安装器及 WDK 工具。第一阶段只从上游固定版本 URL 下载、校验固定 SHA-256，不随 Sunshine 安装包或自有 CDN 再分发。目前只把 Windows 11 24H2 build 26100 记为“已验证”；Windows 10 与其他 Windows 11 build 仍属于实验范围，GUI 不宣称已受支持。这里不把程序集的 `windows10.0.26100.0` API target 误当作已证明的最低 OS 版本；正式分发前必须完成真实 OS build 矩阵并据此决定拒绝安装还是显示实验性警告。

### 3.2 三层生命周期

| 层级 | 例子 | 生命周期所有者 | GUI 能力 |
|---|---|---|---|
| 用户级组件 | Sidecar、HIDMaestro 运行文件、许可证、manifest | GUI 组件管理器 | 安装、校验、更新、修复、删除 |
| 系统级驱动/传输 | UMDF2 虚拟 HID；完整模式另含 usbip-win2 驱动/服务 | Windows Driver Store/SCM/PnP；GUI 仅发起管理 | 检查、提权安装、单独卸载 |
| 串流运行时 | Sidecar 进程、USB attach、虚拟 HID/Audio 设备 | Sunshine Core | 查看状态；仅在未占用时运行测试 |

GUI 退出不得结束由 Core 持有的串流运行时。Core 退出时由带 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job Object 终止 owned Sidecar；owner Named Pipe 的 EOF、`ERROR_BROKEN_PIPE`/`ERROR_NO_DATA`、半开连接最终断开或 owner 进程丢失都会进入 `finally` 清理并销毁该连接创建的所有设备。Sidecar 不作为可脱离 owner 存活的服务。

### 3.3 不复制 Web 串流进程管理细节

Web 串流页面可以复用以下 UX：

- 状态卡片
- 下载及分阶段进度
- 检查更新
- 安装路径、来源和日志入口
- 状态事件加低频轮询对账

DS5 不采用按进程名扫描或 `taskkill` 的做法。虚拟 USB 设备涉及系统状态，应使用明确的 PID、所有者令牌、Named Pipe 和优雅 detach 协议。

### 3.4 外部组件策略

初期将 HIDMaestro 标记为“第三方外部组件”：

- 后端只接受内置 allowlist 中的官方仓库和资产命名规则。
- 发布 manifest 固定版本、下载地址、文件大小和 SHA-256。
- GUI 不接受渲染器传入的任意下载 URL。
- Sunshine 自研 Sidecar 的 `win-x64` 自包含运行时作为独立 Release 资产发布；主安装包只携带同版本 manifest，不再捆绑完整 .NET 运行时。
- GUI 默认按 manifest 下载 Sidecar，也允许用户从任意本地目录选择匹配的官方 ZIP；放在 Sunshine 根目录或 `tools` 目录、文件名保持 `Sunshine.Ds5Sidecar.x64.zip` 的包会被自动发现。Sidecar 与 Portable 资产名刻意不包含 `Windows`，把该标记只留给正式安装器，避免旧版整包更新器第一次升级时误选 ZIP。提权 helper 不接收调用方路径，只接收 allowlist 操作和随机令牌；本地 ZIP 必须通过 manifest 固定的大小与 SHA-256 校验。
- 当前发布物存在 WDK 工具再分发审查事项，因此不放入 Sunshine 安装包或自有 CDN。
- GUI 提供上游项目、许可证、来源 URL 和校验结果。

正式分发前应优先推动上游提供 runtime-only 包，或维护仅剥离非运行期 WDK 工具的最小分支。

### 3.5 HD Haptics 数据路径与自研 SDK 边界

`AlkaidLab/moonlight-audio-haptics` 是“PCM 音频 -> 设备无关触觉意图（IR）”的因果 authoring/down-conversion 引擎，不是 DualSense authored haptics 的无损编解码器。ABI v1 的 80-byte `AhHapticFrame` 只有一组 `continuous_amplitude`、`transient_amplitude`、`sharpness`、`low_band_ratio` 和一个 `stereo_pan`；它不能保存两路触觉 PCM 的逐采样波形、相位与频率内容。

客户端产品层只提供两种互斥路由，不做隐式降级：

```text
客户端选择“真实 DualSense”且连接前发现 USB 四声道端点
  -> 客户端只声明 ML_FF_DS5_HAPTICS_PCM
     -> Sunshine 分帧/时间戳/传输
     -> 客户端抖动缓冲
     -> 物理 DualSense 音频触觉端点（不经过 audio-haptics SDK）

客户端选择“模拟 DualSense”
  -> 客户端只声明 ML_FF_DS5_HAPTICS_IR_V2
     -> Sunshine Core 的 authored-analysis 通道
     -> moonlight-audio-haptics AUTHORED_HAPTIC_STEREO 输入
     -> 双 lane、设备无关 IR v2
     -> 客户端按本机执行器能力渲染
```

common-c 仍使用回调存在性生成底层能力位，但这是应用层模式的协议投影，不是第三种“自动选择”状态。两个回调同时注册属于配置错误，连接启动会失败；非官方客户端同时声明两个位时 Sunshine 固定优先原始 PCM。物理模式预检失败时客户端显示明确警告且不声明 PCM，不能静默切换到模拟模式。

分析器属于 Sunshine Core，不属于 UMDF/USB/IP 驱动或 elevated sidecar。Sidecar 只负责抓取和拆分 channel 3/4；Sunshine 的每个模拟会话维护独立、无分配的分析状态，复用现有有界反馈队列；真实会话不执行分析。目标设备的共振频率、Q 值、振幅下限和播放 API 只在客户端 renderer 中处理，因为这些能力由客户端掌握。

`moonlight-audio-haptics` 通过独立 SDK PR 提供明确的 `AUTHORED_HAPTIC_STEREO` 输入语义和双 lane IR v2；不能复用 ABI v1 的 `stereo_pan` 冒充左右两路。Sunshine 以固定提交的子模块静态链接 SDK，并在传输适配层保证即使空流没有分析输出，也会发送静音 `STREAM_END` 清理客户端执行器。实现借鉴 MPEG Haptics 的独立 channel/curve/wavelet 数据模型和 AOSP HapticGenerator 的执行器标定边界，但不直接嵌入其文件型 Encoder，也不在 Sunshine 端执行设备专用渲染。

## 4. 用户体验设计

### 4.1 页面入口

入口放在 `管理 → 控制器`。`控制器` 是可扩展的设备管理页，DualSense 模拟作为首个功能模块展示；后续控制器驱动、测试工具和映射能力可复用该入口，避免为每种控制器新增一级菜单。

页面沿用现有管理页的标题、卡片密度、按钮和状态样式，不另建一套视觉语言。DualSense 模块仅在需要时展开安装、模式选择、自检和诊断；来源、能力探测及底层传输细节默认折叠。

用户通过“模拟游戏手柄类型”选择 `自动`、Xbox 360、DualShock 4 或 DualSense。
`DualSense` 是唯一的启用入口；DualSense 组件区域只负责安装、修复、HD Haptics、兼容模式、微调和测试，不再维护第二个启用开关。

选择 DualSense 后，如果组件未安装或损坏，页面直接给出“安装组件”或“修复组件”的下一步。Sunshine Core 在组件不可用时统一回退到自动手柄选择，保证串流仍能获得可用手柄，并在日志中记录选择来源与回退原因。

全局手柄类型仍持久化在 `sunshine.conf`，但保存成功后会同时发布到进程内的原子运行时策略。已经创建的虚拟手柄不会在串流中途更换；没有应用级或客户端级覆盖时，新分配的手柄从下一次串流开始使用新选择，无需重启 Sunshine。

本机自检通过后提供 ControllerMeta 入口，用于人工验证按键、摇杆、运动传感器、轮询和普通振动。页面必须明确说明：ControllerMeta 不能验证四声道 PCM、HD Haptics 传输协议或完整的 Sunshine → Moonlight 音频触觉链路。

### 4.2 信息层次

页面按以下顺序呈现：

1. 总体状态和主操作。
2. 组件健康状态。
3. 当前串流或测试会话。
4. 配置项。
5. 来源、安装路径、许可证和日志。

健康状态必须拆分显示，不能用一个“运行中”掩盖部分失效：

| 项目 | 典型状态 | 用户可执行操作 |
|---|---|---|
| 运行组件 | 未安装、已验证、损坏、更新可用 | 安装、修复、更新 |
| USB 传输 | 未安装、正常、已安装但建议重启、版本不兼容 | 安装、按实际探测结果提示重启 |
| 虚拟手柄 | 未连接、枚举中、已连接、被串流使用 | 测试、查看会话 |
| HID 接口 | 未检测、正常、超时 | 重新测试、日志 |
| 四声道音频 | 未检测、正常、端点不匹配 | 修复、日志 |
| HD Haptics | 客户端不支持、待验证、活动 | 客户端能力提示 |

### 4.3 主操作规则

- 未安装：主按钮为“安装组件”。
- 组件已装但传输缺失：主按钮为“安装 USB 传输”，并显示会触发 UAC。
- 就绪：主按钮为“测试虚拟 DualSense”。
- 串流占用：主按钮变为只读状态“正由串流使用”；修复、更新切换和卸载禁用。
- 错误：主按钮为“修复”，旁边保留“查看详情”。
- 更新已下载但正在串流：显示“将在串流结束后安装”，不打断会话。

### 4.4 安装流程

安装对话框应先展示：

- 组件名和版本
- 下载来源
- 是否第三方组件
- 下载大小
- 校验方式
- 下一阶段可能出现的管理员授权

进度采用阶段名称加百分比，不使用只有一条无法解释的进度条：

```text
下载运行组件        42%
校验发布包          等待
安装到暂存目录      等待
检查 USB 传输       等待
验证虚拟设备        等待
```

用户取消下载后删除未完成文件；进入系统驱动安装后，取消只停止后续 Sunshine 操作，不假设 Windows 已回滚。

若 USB/IP 安装程序返回 `3010`，组件安装不能因此中断。Control Panel 继续部署并验证 Sidecar，再以 Sidecar 的实际传输探测决定提示：传输已可用时只建议用户稍后重启；传输未就绪时保留普通 DualSense 控制，并提示重启后再启用 HD Haptics。`3010` 不是阻止安装完成的错误状态。

### 4.5 测试流程

“测试虚拟 DualSense”创建有边界的测试会话：

1. 启动或连接 Sidecar。
2. attach 一个测试设备。
3. 等待 HID 接口，建议超时 8 秒。
4. `dualsense-composite` 等待四声道音频端点（建议超时 12 秒）并允许 PCM 验证；`dualsense` 跳过该等待并明确报告“此 profile 不支持四声道音频”。
5. 显示手柄输入和反馈状态。
6. 可执行一次短促、低强度的左右通道触觉测试；执行前给出明确按钮，不自动播放。
7. 用户结束、关闭页面或超时后 detach 测试设备。

测试对话框不得宣称“HD Haptics 正常”，除非已观察到通道 3/4 的有效 PCM 数据并完成客户端回传确认。只有端点枚举成功时显示“四声道音频端点可用”。

### 4.6 串流时的页面行为

页面显示：

- 所有者：`Sunshine 串流会话`
- 应用名和客户端名（如可用）
- 虚拟设备数量
- HID、音频及 haptics 当前活动状态
- 会话开始时间

串流中不显示“停止设备”按钮，避免 GUI 误中断用户会话。需要终止时引导用户结束对应串流。

### 4.7 错误呈现

错误提示采用“发生了什么 + 影响 + 下一步”，并保留稳定错误码用于支持：

> 四声道音频端点在 12 秒内未出现。普通手柄输入仍可使用，但 HD Haptics 当前不可用。请尝试修复组件。`DS5-AUDIO-002`

禁止直接把 Rust、HRESULT 或 Win32 原始错误堆栈作为用户主文案。详细信息放入可复制区域和日志。

## 5. 状态模型

### 5.1 聚合状态

```rust
#[serde(rename_all = "snake_case")]
enum Ds5OverallState {
    NotInstalled,
    Installing,
    TransportMissing,
    Ready,
    Testing,
    InUse,
    UpdatePending,
    RepairRequired,
    Error,
}
```

聚合状态只用于页面标题和主按钮。组件、传输和运行时仍分别返回状态。

### 5.2 状态快照

```rust
struct Ds5StatusSnapshot {
    revision: u64,
    overall: Ds5OverallState,
    component: ComponentStatus,
    transport: TransportStatus,
    runtime: RuntimeStatus,
    capabilities: Ds5Capabilities,
    operation: Option<OperationSnapshot>,
    last_error: Option<Ds5Error>,
}

struct ComponentStatus {
    state: String,
    installed_version: Option<String>,
    available_version: Option<String>,
    verified: bool,
    install_path: Option<String>,
    source_url: Option<String>,
    update_pending: bool,
}

struct TransportStatus {
    state: String,
    version: Option<String>,
    signed: Option<bool>,
    reboot_required: bool,
}

struct RuntimeStatus {
    state: String,
    pid: Option<u32>,
    protocol_version: Option<u32>,
    owner: Option<String>,
    session_id: Option<String>,
    device_count: u32,
}

struct Ds5Capabilities {
    hid: String,
    audio_4ch: String,
    hd_haptics: String,
    client_hd_haptics: Option<bool>,
}
```

`revision` 必须单调递增，渲染器忽略旧事件，避免轮询响应覆盖较新的事件。

### 5.3 运行状态机

```text
ready
  -> starting       首个 DS5 会话或 GUI 测试
  -> attached       USB、HID 已枚举
  -> in_use         串流开始；或 testing
  -> grace_period   最后一个串流会话结束
  -> stopping       宽限期到或测试结束
  -> ready
```

最后一个正式会话结束后建议保留 10 秒宽限期。宽限期内同一配置的新会话可以复用设备，减少 Windows 设备插拔和游戏重新识别。

## 6. 控制接口

### 6.0 控制器全局配置

Control Panel 不持有或回写完整的 `/api/config` 快照。它通过以下受认证接口只读写控制器字段：

```text
GET  /api/gamepad/config  -> gamepad、DS4 行为与 DSU 设置
POST /api/gamepad/config  -> 仅包含本次变化字段的部分更新
```

Core 只接受固定字段集合，并在 `config_file_mutex` 内把部分更新合并到 `sunshine.conf`。因此 Panel 修改 `gamepad` 时不会覆盖同时存在的显示、编码器、网络、证书或其他配置。`gamepad` 保存成功后在同一临界区发布运行时策略；其他控制器高级设置仍按各自现有生命周期生效。

### 6.1 GUI/Tauri 命令

```text
ds5_get_status() -> Ds5StatusSnapshot
ds5_check_release() -> ReleaseInfo
ds5_install_component(operation_options) -> OperationAccepted
ds5_cancel_operation(operation_id) -> CancelResult
ds5_install_transport(operation_id) -> OperationAccepted
ds5_repair(operation_options) -> OperationAccepted
ds5_start_test(test_options) -> TestSession
ds5_stop_test(test_session_id) -> StopResult
ds5_uninstall_component() -> OperationAccepted
ds5_uninstall_transport(confirm_system_scope) -> OperationAccepted
ds5_open_logs() -> ()
ds5_open_install_path() -> ()
```

渲染器不得传入可执行文件路径、任意 URL、命令行或驱动 INF 路径。后端从已校验 manifest 解析这些值。

### 6.2 GUI 事件

```text
ds5-status-changed       带 revision 的完整 Ds5StatusSnapshot
ds5-operation-progress   operation_id、stage、progress、message_key
ds5-test-feedback        test_session_id、HID/Audio/Haptics 测试结果
```

v1 事件只发送完整快照，不定义增量合并或字段清除语义。事件是主要更新机制；页面每 10 秒执行一次 `ds5_get_status` 对账。渲染器记录最近 `revision`，丢弃 revision 不大于当前值的事件和轮询响应，避免较慢的轮询覆盖较新的事件。页面重新打开时通过 operation snapshot 恢复进度，不依赖 Vue 组件一直存活。

### 6.3 Core/Sidecar 协议

建议使用带 ACL 的本地 Named Pipe，例如：

```text
\\.\pipe\sunshine-ds5-v1
```

最小消息集：

```text
hello(protocol_version, diagnostic_process_identity)
probe()
attach(session_id, device_index, feature_flags)
update_input(session_id, report)
subscribe_output(session_id)
detach(session_id)
get_status()
shutdown(owner_token)
```

协议要求：

- 长度前缀和最大消息尺寸。
- 协议版本协商。
- 每个 attach 使用不可预测 session ID。
- OS Named Pipe 客户端身份在连接建立时绑定为 owner；不信任 `diagnostic_process_identity` 等客户端提交字段做授权。
- `attach`、`update_input`、`subscribe_output` 和 `get_status` 只接受当前连接 owner；Core 持有 owner token，GUI 测试使用独立、低权限 test token。
- Sidecar 拒绝非所有者 detach 和 shutdown；连接断开会清理该 owner 创建的全部设备。
- 输出报告和音频数据使用有界队列；控制消息不得被高频数据饿死。
- 已实现的 owner 校验（v1）：管道 ACL 限定当前用户 + Sidecar 在连接建立时校验客户端进程的提权状态，非提权客户端拒绝并断开、继续等待真正的 owner（不因被抢连而退出，避免单次抢连导致该会话分配失败）；同用户非提权进程即使抢到单实例管道也无法驱动 elevated Sidecar。GUI 低权限 test token 仍属后续工作。
- 已实现的停滞保护（v1）：Core 对数据面写操作设置 5 秒停滞上限；写停滞会取消 reader 的挂起读取并进入既有的单次恢复路径，sidecar 读循环阻塞不再冻结 Sunshine 输入线程。

高频四声道音频数据不应经 Tauri 或 JSON 传输。后续实现使用共享内存环形缓冲区或专用本地数据通道；Named Pipe 只负责控制和状态。

## 7. 文件与模块规划

### 7.1 Control Panel

建议新增：

```text
src_assets/common/sunshine-control-panel/
  src-tauri/src/ds5/
    mod.rs
    commands.rs
    manager.rs
    manifest.rs
    installer.rs
    transport.rs
    probe.rs
  src/renderer/components/DualSenseSettings.vue
  src/renderer/components/ds5/Ds5HealthList.vue
  src/renderer/components/ds5/Ds5TestDialog.vue
```

同时在以下位置注册：

- `src-tauri/src/main.rs`：Tauri commands。
- `src-tauri/src/app.rs`：仅清理 GUI 自己创建的测试 session；不得杀死 Core session。
- `src/renderer/tauri-adapter.js`：类型稳定的 DS5 adapter。
- desktop i18n：所有用户文案使用 key，不拼接英文后端错误。

为减少回归，第一阶段不重构 Moonlight Web 模块。可抽取新的下载校验和 operation snapshot 基础设施供 DS5 使用，稳定后再决定是否迁移 Web 串流。

### 7.2 Sunshine Core

建议新增：

```text
src/platform/windows/ds5/
  ds5_manager.h/.cpp
  ds5_sidecar_client.h/.cpp
  ds5_session.h/.cpp
  ds5_audio_transport.h/.cpp
```

输入创建路径根据请求设备类型选择：

```text
X360 / DS4 -> 现有 ViGEm 路径
DS5        -> 组件可用时 Ds5Manager -> Sidecar
           -> 组件不可用时回退到自动选择
```

Core 维护引用计数，按 session ID 管理多客户端。第一阶段可以限制一个 DS5 设备。只有组件不可用时允许在创建前回退到自动选择；Sidecar 已开始创建后发生的运行错误仍应返回明确错误，不能在设备已部分建立时悄悄切换类型。

### 7.3 安装目录

```text
<Sunshine install>\tools\sunshine-ds5-component\
  active\
  previous\
  staging-<operation-id>\
  staging-<operation-id>.partial
```

下载与 SHA-256 校验由 Control Panel 完成，但只有以管理员身份重启后才能写入上述受保护目录或执行自检。不得从 `%LOCALAPPDATA%`/`%TEMP%` 等用户可写路径提权执行 sidecar 或 wrapper，避免 TOCTOU 替换。系统驱动保持其标准 Driver Store/服务位置，不复制进上述目录。

## 8. 安装、更新、回滚与卸载

### 8.1 发布 manifest

建议由 Sunshine 版本内置受信任 manifest，而不是运行时信任 GitHub `latest` 返回内容：

```json
{
  "schema": 1,
  "component": "hidmaestro-runtime",
  "version": "example",
  "url": "https://github.com/OWNER/REPO/releases/download/TAG/ASSET.zip",
  "sha256": "PINNED_SHA256",
  "size": 0,
  "entrypoint": "sunshine-ds5.exe",
  "protocol": 1,
  "licenses": ["LICENSE-HIDMaestro", "LICENSE-usbip-win2"]
}
```

将来需要不随 Sunshine 发版更新 manifest 时，必须对远端 manifest 做独立签名验证，并实现回滚保护。

### 8.2 原子安装

1. 下载到 `downloads/<operation-id>.partial`。
2. 校验总大小和 SHA-256。
3. 解压到唯一 staging 目录。
4. 拒绝绝对路径、`..`、重解析点和超出尺寸/文件数限制的归档内容。
5. 校验必须文件、许可证和协议版本。
6. 执行 `probe`，不得 attach 正式设备。
7. 将 staging 重命名为 `versions/<version>`。
8. 原子替换 `active.json`。
9. 失败时保持旧 active 版本不变。

### 8.3 更新

- 允许后台下载和校验。
- `runtime.owner == core` 时只设置 `update_pending`。
- 最后一个串流结束且 runtime 停止后切换 active 版本。
- 新版本首次 probe 失败时恢复旧 `active.json` 并记录回滚原因。
- 只保留当前版本、上一可用版本和正在暂存的版本。

### 8.4 修复

修复不是盲目重装，按顺序执行：

1. 检查 active manifest。
2. 校验组件文件摘要。
3. probe Sidecar 协议。
4. 检查 USB/IP 服务和设备接口。
5. 清理由 Sunshine owner token 标识的孤立测试会话。
6. 做一次可取消的枚举测试。
7. 仅重装失败的用户级组件；系统传输需要用户再次明确授权。

### 8.5 卸载

- 串流占用时禁止卸载，并显示占用会话。
- 先停止 GUI 测试，再执行 Sidecar detach。
- 删除用户级组件时不自动删除 usbip-win2。
- “卸载 USB 传输”放在高级/危险操作区，说明它可能影响其他使用该驱动的软件，并要求二次确认和 UAC。

## 9. 安全与可靠性要求

1. 下载源 allowlist、固定摘要和 HTTPS 缺一不可。
2. 归档解压必须防 Zip Slip、解压炸弹、重解析点和文件覆盖。
3. 安装器只操作固定组件根目录，删除前解析并验证绝对路径仍位于该根目录。
4. Sidecar 启动路径必须来自已验证 active manifest。
5. 不把管理员权限传给常驻 Sidecar；提权 helper 只执行单个、结构化的驱动操作。
6. Named Pipe ACL 仅允许当前用户、Sunshine 服务身份和管理员访问；Sidecar 在连接建立时校验客户端进程已提权，拒绝非提权连接。
7. 不按可执行文件名全局终止进程。
8. 每个异步操作只有一个 writer，并有 operation ID、取消状态和可恢复 snapshot。
9. GUI 收到的错误文本视为不可信数据，显示时转义；用户文案由稳定错误码映射。
10. 日志不得记录输入报告原始数据、用户令牌或完整本地敏感路径。

## 10. 稳定错误码

| 错误码 | 含义 | 默认恢复动作 |
|---|---|---|
| `DS5-PKG-001` | 下载摘要不匹配 | 删除下载并重试 |
| `DS5-PKG-002` | 发布包结构无效 | 停止安装并查看日志 |
| `DS5-PKG-003` | Sidecar probe 失败 | 回滚或修复 |
| `DS5-PKG-004` | 提权 helper 启动、授权或 IPC 失败 | 重试 UAC；不修改现有组件 |
| `DS5-PKG-005` | 本地 Sidecar 包与当前 manifest 不匹配 | 下载同一 Sunshine Release 的组件 ZIP |
| `DS5-DRV-001` | USB 传输缺失 | 提示安装 |
| `DS5-DRV-002` | 用户取消 UAC | 保留组件，稍后安装 |
| `DS5-USB-001` | attach 失败 | 清理测试会话后重试 |
| `DS5-HID-001` | HID 枚举超时 | 修复或日志 |
| `DS5-AUDIO-002` | 四声道音频枚举超时 | 降级为普通反馈并提示 |
| `DS5-RUN-001` | Sidecar 崩溃 | 一次自动恢复，随后报错 |
| `DS5-RUN-002` | 组件正被串流占用 | 等待会话结束 |
| `DS5-PROTO-001` | Core/Sidecar 协议不兼容 | 更新组件或 Sunshine |

## 11. 配置项

第一阶段只暴露必要选项：

- `模拟游戏手柄类型`：全局选择的唯一来源；显式选择 `DualSense` 才使用 DS5，`自动` 不主动探测或选择 DS5。
- `串流结束后保留设备`：默认 10 秒，可选 0、10、30 秒。
- `启用 HD Haptics 音频通道`：默认自动；客户端不支持时不发送。
- `诊断日志`：默认普通，仅临时启用详细模式。

`ds5_config.json` 只保存 HD Haptics、兼容模式和振动微调，不保存手柄类型或 DS5 启用状态。旧文件中的 `ds5_enabled` 仅在读取时忽略；下一次保存会将其移除，不据此修改 `sunshine.conf`。升级后用户需要在“设备中心 → 控制器 → 模拟游戏手柄类型”自行重新确认所需类型。

以下内容不应暴露给普通用户：USB/IP 端口、VID/PID、内部 Pipe 名称、原始 USB 描述符、Sidecar 命令行。

## 12. 可访问性与本地化

- 状态不能只依靠红/绿颜色，必须包含图标和文字。
- 进度条提供当前阶段和百分比的可读文本。
- 所有操作均可键盘访问；焦点在对话框关闭后返回触发按钮。
- 动态状态使用适度的 `aria-live="polite"`，错误不要重复播报。
- 避免在中文里直接拼接版本、路径和错误句子；使用带占位符的 i18n key。
- “HD Haptics”“ViGEm”“HIDMaestro”等产品或技术名保持一致，不作不同页面的自由翻译。

## 13. 实施阶段

### Phase 0：契约冻结

- 冻结 Sidecar v1 控制协议和状态模型。
- 确认 runtime-only 发布物策略及许可证清单。
- 产出固定测试版本 manifest。

完成标准：离线 mock Sidecar 能通过状态、attach、detach 和 crash 测试。

### Phase 1：组件管理与 GUI

- 实现下载、摘要校验、安全解压、原子安装、状态快照。
- 实现页面的未安装、安装中、传输缺失、就绪和错误状态。
- 实现来源/许可证/日志入口。

完成标准：不安装驱动也能安全完成组件安装、校验、修复和卸载。

### Phase 2：本机测试生命周期

- 接入传输检查与提权安装 helper。
- 实现 GUI test session。
- 验证 HID 和四声道音频端点。
- 页面关闭和 GUI 异常退出均能清理测试设备。

完成标准：连续执行 50 次测试 attach/detach，无孤立设备、进程或 handle 增长。

### Phase 3：Sunshine 串流接入

- DS5 类型路由到 `Ds5Manager`。
- 实现引用计数、所有者 token、宽限期和重连复用。
- 接入输入报告及输出反馈。
- GUI 显示串流占用且禁止破坏性操作。

完成标准：串流断开、客户端崩溃、Core 重启和网络重连均可恢复。

### Phase 4：四声道音频与 HD Haptics

- 捕获虚拟音频端点通道 3/4。
- 通过有界低延迟数据通道发送到客户端。
- 增加客户端能力协商、静音检测和遥测。
- 没有 haptics 数据时不伪报“活动”。

完成标准：支持游戏中可观测到 ch3/4 PCM，客户端能稳定重放，普通音频和输入不受影响。

### Phase 5：发布加固

- 完成第三方发布物法律审查。
- 完成更新回滚、签名/摘要失效演练和恢复文档。
- 收敛诊断日志并补齐多语言文案。

## 14. 测试矩阵

| 场景 | 预期结果 |
|---|---|
| 无组件首次打开页面 | 明确显示未安装，不触发 UAC |
| 下载中关闭页面再打开 | 从 operation snapshot 恢复进度 |
| SHA-256 不匹配 | 删除 partial，不污染 active 版本 |
| 用户取消 UAC | 组件保留，状态为传输未安装 |
| 驱动安装程序返回 `3010` | 继续部署并验证组件；根据实际传输探测建议重启，不重复安装 |
| GUI 测试期间关闭页面 | 只清理 test session |
| 串流期间关闭 GUI | 串流设备继续工作 |
| 串流期间点击更新 | 下载并校验，延迟切换 |
| 串流期间点击卸载 | 禁止并显示占用会话 |
| Sidecar 启动后崩溃 | Core 清理状态并最多自动恢复一次 |
| Core 异常退出 | Job Object/watchdog 释放设备 |
| 10 秒内重新连接 | 复用设备，避免重新枚举 |
| 旧 Sidecar 协议 | 阻止 attach，提示升级 |
| HID 正常、Audio 超时 | 普通控制可用，HD Haptics 明确降级 |
| 已选择 DS5、组件未安装或损坏 | GUI 提示安装/修复；Core 记录告警并按自动模式创建可用手柄 |
| 多用户 Windows 会话 | 非所有者不能 detach 当前设备 |

## 15. 验收标准

功能验收：

- GUI 可完整管理用户级组件生命周期。
- 系统级传输和用户级组件可以独立安装、修复和卸载。
- 串流 runtime 由 Core 管理，GUI 只观察且不能误杀。
- ViGEm 路径没有行为变化。
- HID、四声道音频和 HD Haptics 状态可分别诊断。

UX 验收：

- 用户在任一状态都能看到唯一明确的推荐下一步。
- UAC、重启、第三方来源和系统级卸载的影响在执行前说明。
- 安装失败不会留下看似“已就绪”的假状态。
- 技术错误可复制，但默认文案不要求用户理解 USB/IP、HRESULT 或设备描述符。

可靠性验收：

- 50 次测试 attach/detach 无残留。
- 100 次短连接/重连无失控进程和重复设备。
- 更新失败能自动回滚到最后可用版本。
- GUI、Core、Sidecar 三种异常退出均有明确清理和恢复路径。

## 16. 后续决策点

以下事项应在 Phase 0 结束前确认：

1. Sidecar 是调用 HIDMaestro 公共 API，还是消费上游提供的 runtime-only executable。
2. usbip-win2 的精确版本、签名验证规则和安装/卸载命令契约。
3. Sunshine 服务模式下 Sidecar 的用户会话及音频端点可见性。
4. 第一阶段是否只允许一个虚拟 DS5。
5. 客户端 HD Haptics 能力协商字段和数据封包格式。

## 17. 首期落地决策与验证记录

截至 2026-08-14（Asia/Hong_Kong），首期实现已经冻结以下决策：

1. Sidecar 调用 HIDMaestro v1.6.2 的公共 `HIDMaestro.Core.dll` API，不复制其实现，也不调用 HIDMaestroTest UI。
2. HIDMaestro 使用官方发布物 `HIDMaestro-v1.6.2.zip`，固定下载地址和 SHA-256 `6ae8df0cf317baf7e65777e2929f618916a67831b5ff1162205310f2c08b80ff`。发布物大小为 118,881,819 bytes。
3. Sunshine 主安装包仅携带自研 Sidecar 组件包的固定 manifest，不携带完整 .NET 运行时或 HIDMaestro DLL。Sidecar 自包含 ZIP 作为同一 Release 的独立资产发布；GUI 在用户明确选择安装时下载或读取用户选择的匹配 ZIP，并继续单独下载、校验 HIDMaestro 后只提取 Core、许可证、README 和第三方通知。
4. 首期每个 Sunshine 进程只允许一个虚拟 DualSense。Xbox 360、DualShock 4 和既有自动模式继续走 ViGEm，不改变成熟驱动支持范围。
5. 客户端只选择 `physical` 或 `emulated`。前者预检 USB DualSense 四声道端点后声明 `ML_FF_DS5_HAPTICS_PCM`；后者声明 `ML_FF_DS5_HAPTICS_IR_V2`。两位互斥且不在运行中自动切换。
6. `0x550A` v1 固定承载 48 kHz、双声道、S16LE 原始 PCM；`0x550B` v2 固定承载 72-byte 双 lane IR。两者均按 5 ms 节拍使用不可靠有序传输，断序/`DISCONTINUITY` 重置客户端状态。
7. 原始第 3/4 声道触觉 PCM 只在模拟模式进入 `moonlight-audio-haptics` authored stereo API。IR 是有损、设备无关的振动意图，不宣传为原始 HD Haptics 等价物；设备标定和最终 actuator renderer 始终位于客户端。

### 17.1 已完成的本机验证

- Sunshine Windows UCRT 完整 Release 链接通过，新 DS5 backend 和控制流进入最终 `sunshine.exe`。
- Sidecar .NET Release 编译零警告、零错误；协议自测覆盖 hello、attach、输入、触摸、运动、电池、detach 与 owner 断开清理。
- 提权复合 profile 自测通过，HID、四声道音频端点和设备清理均成功。
- 强端到端环路通过：Moonlight 实际 WASAPI 渲染器写入 HIDMaestro 48 kHz 四声道端点，Sidecar 从 channel 3/4 收到 960 bytes 非静音触觉 PCM。
- Moonlight Qt 6.9.2 / MSVC Release 完整链接通过；客户端使用自适应端点容量的 15 ms 上限预缓冲，20 ms 饥饿或序号不连续时重置。
- Sidecar `win-x64` 自包含发布通过，未压缩产物约 107 MB 且确认不包含 `HIDMaestro.Core.dll`；该运行时被压缩为独立、固定 SHA-256 的组件资产，把官方校验 DLL 放入 staging 后，probe 返回协议 1、standard/composite profile、驱动及 USB/IP 均可用。
- Control Panel 已同步 Sunshine master 使用的 VDD/HDR 基线；3 项 DualSense Rust 单测、Vue production build 和 12 项 renderer 测试通过。完整配置读取失败会中止保存，USB/IP 不可用时后端拒绝 composite profile，页面仍允许用户切回 HID-only。
- Core 的 DS5 命名管道改用 overlapped I/O；独立 fake-sidecar 回归测试覆盖 `alloc -> reader blocked -> free`，本机在 93 ms 内完成，避免同步 `ReadFile` 与 owner EOF 相互等待。
- Windows `application` 组件的隔离安装烟测通过：主包只安装 `tools/ds5-sidecar-package.json`，不再出现 `tools/sunshine-ds5-sidecar/Sunshine.Ds5Sidecar.exe`；GUI 下载或选择本地组件包后才把 Sidecar 与经校验的 HIDMaestro Core 激活到组件目录。
- `moonlight-audio-haptics` authored/ABI 测试、common-c IR v2 golden parser 和 Moonlight IR-to-rumble renderer 测试通过。
- Sunshine 合成 PCM -> SDK 双 lane IR -> 72-byte 小端序列化 -> common-c 解析的跨仓库测试通过；空流结束会产生静音 `STREAM_END`。

### 17.2 首期明确限制

- GUI 的复合 profile 测试需要 UAC；用户取消授权时保留用户级组件，不反复弹出授权。
- HIDMaestro/usbip-win2 仍是外部测试签名路线，不属于 Sunshine 后续实体证书的 WHQL/Attestation 范围。
- 多虚拟 DS5、设备到音频端点的稳定一一映射、服务会话到交互用户音频会话的跨 session 代理留到下一阶段。
- 模拟模式首期只映射到 SDL 的低频/高频振动电机，不宣称还原物理 DualSense 的逐采样 HD Haptics；后续设备专用 renderer 必须保持在客户端，并沿用同一 IR 协议。
