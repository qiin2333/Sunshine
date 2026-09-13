# 后处理链（Post-Processing Chain）设计与实施规划

## 1. 定位

把 RTX HDR（#1018）验证过的"单 backend pre-encode 滤镜"泛化为**自由插件链**：
用户可向管线中插入任意符合 `backend_abi` v2 的处理 DLL（VSR、TrueHDR、LSFG、AFMF、
第三方锐化等），Sunshine 在会话初始化时校验整条链并**生成确定性的执行计划**
（域转换自动补边、时序节奏、失败降级矩阵）。校验器保证**可达、合法、代价可见**；
不保证任意组合的画面质量——组合质量是用户自由。

配套视觉稿：`docs/postprocess_chain_mockup.html`（本文 §8 的实现级原型）。

## 2. 核心决策

### 2.1 用户链夹在两条固定适配腿之间

用户**不需要**产出编码器要求的域。链的两端由 Sunshine 固定桥接：

- **捕获适配腿**：把捕获原生帧（VDD/DDA 契约，如 `BGRA8 · sdr_rec709`）喂给链首；
- **编码适配腿**：把链尾输出转成编码器所需域（HDR 流 `P010 · pq_bt2020` / SDR 流 `NV12`），
  并负责**插值帧的元数据合成**（见 §2.3）。

域不衔接时由校验器自动插入内置转换边（§5.1）。这是"自由插入"成立的前提：
用户 stage 只需声明自己吃什么、吐什么。

### 2.2 校验器产出确定性执行计划

校验器做三件事：**图可达性**（域链能否接通）、**约束检查**（ABI/分辨率/时序上限）、
**自动补边与钩子注入**（转换、IDR reset、元数据合成）。输出一份确定的
`plan`（§6），会话按 plan 执行；WebUI 按 plan 逐槽上报状态（沿用 #1031 的引导模式）。
校验结果分三档：接受 / 接受并警告 / 拒绝（附原因）。边界声明：校验器不保证
任意组合的画质收益，只拦截**跑不起来或代价不可接受**的组合。

### 2.3 时序（temporal）是唯一有状态契约

空间类 stage（VSR/HDR/锐化）无状态、1 进 1 出；时序类（插帧）需要历史帧，
输出帧数 > 输入（如 N→2N）。语义约定：

- temporal stage **之后**的所有 stage（含内置转换与编码适配腿）以放大后的帧率运行，
  代价前置警告（R5）；
- 生成帧打 `interpolated` 类型标签，随帧流经下游；元数据合成在编码适配腿完成
  （L1/MaxSCL 取相邻真帧插值），下游注入机制（DV RPU / HDR10+ SEI 按 frame_index
  工作）零改动；
- IDR/会话重建 → 宿主调用所有 temporal stage 的 `reset()`，插值输出在凑齐两帧
  真帧前挂起（瞬态帧率减半，客户端既有 reinit 容忍已覆盖）。

### 2.4 信任边界：进程内任意 DLL

加载任意 DLL = 进程内任意代码，与运行 Sunshine 本体同级信任。per-stage bypass
能兜住"返回失败"类故障（摘除该节重新生成计划），**挡不住进程级崩溃**
（SEH 接住 AV 后状态已不可信）。文档必须明示这条边界。探测（加载读 caps）
会执行 DllMain：只发生在用户显式添加 DLL 或会话启动加载已配置链时，
**绝不自动扫描执行未知 DLL**。

### 2.5 兼容迁移：rtx_hdr → 单元素链

现有 per-app `rtx_hdr` 配置自动迁移为单元素链
`[foundation_truehdr_backend.dll]`；v1 ABI backend（无 caps 导出）按
`{等分辨率, 非 temporal, 域由现有配置推导}` 合成 caps。**Phase 1 的回归基线 =
TrueHDR 以链模式跑通且现有全部测试绿**。

## 3. 管线架构

```
┌──────────┐   ┌────────────── 用户链（自由插入，按序执行）──────────────┐   ┌──────────────┐
│ 捕获适配腿 │ = │  [用户DLL] → [自动转换?] → [用户DLL] → …               │ = │  编码适配腿    │
│ 捕获原生域 │   │  每节自报 caps：输入域/格式 → 输出域/格式 · 分辨率行为  │   │  域转换+编码域 │
└──────────┘   └─────────────────────────────────────────────────────┘   │  +插值帧元数据 │
                                                                          └──────────────┘
失败降级矩阵（per-stage，独立）：FG 失败回原帧率；VSR 失败回 bilinear；
HDR 失败回 SDR-in-PQ mock。摘除后重新生成 plan，链断裂则继续降级直至最小链。
```

## 4. ABI v2 完整草案

### 4.1 兼容原则

- v2 导出 `foundation_stage_get_api`；loader 先探测 v2，再回退探测 v1 的
  `foundation_truehdr_get_api`（合成 caps：等分辨率、非 temporal）。
- v1 backend（现有 foundation_truehdr_backend.dll）**不改**即可入链。

### 4.2 头文件草案

```c
#define FOUNDATION_STAGE_ABI_VERSION 2

typedef enum { FRAME_TYPE_REAL = 0, FRAME_TYPE_INTERPOLATED = 1 } frame_type_e;

typedef enum {
  STAGE_RESOLUTION_SAME = 0,      // 输出宽高 == 输入
  STAGE_RESOLUTION_SCALE = 1,     // min_scale..max_scale 倍
  STAGE_RESOLUTION_ARBITRARY = 2, // 任意（需与编码适配协商）
} stage_resolution_e;

typedef struct stage_caps_t {
  uint32_t struct_size;
  uint32_t abi_version;                 // FOUNDATION_STAGE_ABI_VERSION
  const char *name;                     // "nvidia.vsr" / "lsfg" / "amd.afmf"
  /* 域契约 */
  uint32_t input_domain;                // frame_domain_e
  uint32_t input_encoding;              // pixel_encoding_class_e
  uint32_t output_domain;
  uint32_t output_encoding;
  /* 分辨率 */
  uint32_t resolution_behavior;         // stage_resolution_e
  float    min_scale;
  float    max_scale;
  /* 时序 */
  uint32_t temporal;                    // 需要历史帧
  uint32_t max_frames_out;              // 每次 process 的输出槽上限（≥1）
  /* 参数（WebUI 动态表单） */
  const char *(*describe_params)(void);          // 返回 JSON Schema
  int         (*apply_params)(void *inst, const char *json);
} stage_caps_t;

typedef struct stage_api_t {
  uint32_t struct_size;
  uint32_t abi_version;
  const stage_caps_t *(*caps)(void);
  foundation_status_e (*create)(const stage_create_params_t *params, void **instance);
  /* 时序/非时序统一入口：非 temporal 后端忽略 out_types 且 out_count 恒 1 */
  foundation_status_e (*process)(void *instance,
                                 const gpu_frame_view_t *input,
                                 gpu_frame_view_t *out_frames,     // 宿主按 caps 分配
                                 frame_type_e *out_types,
                                 uint32_t *out_count);
  void (*reset)(void *instance);                 // IDR/重建：清历史帧
  void (*flush)(void *instance);
  void (*destroy)(void *instance);
} stage_api_t;

typedef struct stage_create_params_t {
  ID3D11Device *device;
  ID3D11DeviceContext *context;
  uint32_t input_width, input_height;            // 会话实际输入
  float    scale_hint;                           // 编码适配腿期望的输出倍率（可为 1.0）
  const char *params_json;                       // per-app 参数
} stage_create_params_t;

// 唯一导出
const stage_api_t *foundation_stage_get_api(uint32_t abi_version);
```

### 4.3 宿主侧行为

- 输出纹理槽由宿主按 `caps.max_frames_out` 分配并复用（池化），后端只在
  尺寸/域变化时请求重建（同 v1 语义）；
- `process` 串行调用（沿用 external_backend_mutex 模式，per-stage 一把锁）；
- 首帧失败即按 R8 策略摘除/降级（沿用 failover 语义）。

## 5. 域转换图与校验规则

### 5.1 内置域转换图（计划生成素材库）

```text
节点（domain · encoding）：
  N1 = sdr_rec709 · unorm8        （BGRA8，捕获原生）
  N2 = linear_scrgb · float16     （FP16 scRGB）
  N3 = pq_bt2020 · unorm10        （P010，编码目标·HDR）
  N4 = sdr_rec709 · nv12          （NV12，编码目标·SDR）

内置边（现成 GPU pass，任意边可附 bilinear 缩放）：
  N1 → N2  linearize（现有中性 shader）
  N2 → N3  PQ 转换（现有）
  N2 → N4  SDR 转换（现有）
  N1 → N4  直转（现有）
```

### 5.2 校验规则

| 规则 | 检查 | 结果 |
|---|---|---|
| R1 ABI | `get_api` 版本 ∈ 支持集 | ✗ 拒绝（报版本差） |
| R2 域链 | 相邻 stage 域相同→直连；不同→内置转换图可达性 | 不可达 → ✗（报缺哪条边） |
| R3 分辨率 | 链尾输出可被编码适配腿接受；scale ∈ caps 范围且 ≤ 编码上限 | ✗（报上限） |
| R4 时序 | temporal 段 >1 → 强警告；帧率乘积 > 编码上限 | ⚠ / ✗ |
| R5 成本 | temporal 之后的段帧率 ×N | ⚠ 前置告知 |
| R6 HDR 语义 | 编码 HDR 但链中无 sdr→scRGB 等效变换 | ⚠（允许 SDR-in-PQ，明示） |
| R7 IDR | 存在 temporal → 自动注入 reset 钩子 | 自动 |
| R8 失败策略 | per-stage `bypass`：摘除该节后**重新生成 plan** | 链断裂则继续降级 |

## 6. 运行计划（plan）

```json
{
  "stages": [
    { "slot": 0, "kind": "capture_adapter", "io": "bgra8_sdr·1080p" },
    { "slot": 1, "kind": "dll", "name": "nvidia.vsr", "dll": "…\\nvidia_vsr.dll",
      "io": "bgra8→bgra8", "res": "1080p→4K", "state": "active" },
    { "slot": 2, "kind": "dll", "name": "nvidia.truehdr", "dll": "…\\foundation_truehdr_backend.dll",
      "io": "bgra8→fp16_scrgb", "res": "4K", "state": "active" },
    { "slot": 3, "kind": "dll", "name": "lsfg", "temporal": true,
      "io": "fp16_scrgb→fp16_scrgb", "frames": "×2", "state": "active" },
    { "slot": 4, "kind": "builtin_convert", "io": "fp16_scrgb→p010_pq·4K" },
    { "slot": 5, "kind": "encoder_adapter",
      "frame_metadata": "synthesize_interpolated" }
  ],
  "pacing": { "input_fps": 60, "output_fps": 120 },
  "bitrate_estimate": 2.1,
  "warnings": ["temporal_chain_x2"],
  "excluded": [ { "dll": "my_sharpen.dll", "reason": "abi_1_unsupported" } ]
}
```

## 7. 配置、发现与信任边界

per-app（apps.json），旧 `rtx_hdr=on` 自动迁移为单元素链：

```json
"postprocess": {
  "chain": [
    { "dll": "C:\\backends\\nvidia_vsr.dll" },
    { "dll": "C:\\backends\\foundation_truehdr_backend.dll",
      "params": { "peak_nits": 1000 } },
    { "dll": "C:\\backends\\LSFG.dll" }
  ],
  "on_stage_failure": "bypass"
}
```

发现：WebUI 只**列出** `<Sunshine>/postprocess/` 目录中的文件名（不加载）；
用户显式添加 → 会话启动时加载探测（执行 DllMain）→ caps 进校验器 →
结果（✓/⚠/✗+原因）写回 UI 并缓存到配置。探测失败不留半初始化状态。

## 8. WebUI

实现级视觉稿：`docs/postprocess_chain_mockup.html`。要点：链轨道（rail）为视觉
主体——节点圆点 + 连接线 + 域边标签（mono 字体）；时序段带 `×2` 警示徽章；校验
失败段显示排除态（划名 + 原因）；底栏校验摘要 + 应用按钮；下方独立的运行时状态
面板（逐槽 state/backend/耗时，沿用 #1031 的 hint 引导模式）。

## 9. 分阶段计划

| 阶段 | 内容 | Go/No-Go |
|---|---|---|
| 0 | ABI v2 定稿（本文 §4 评审）+ 校验器规则评审（§5） | 设计评审过 |
| 1 | 链执行器 + 内置转换边 + `rtx_hdr`→单元素链自动迁移 | TrueHDR 链模式跑通且现有全部测试绿（执行器回归基线） |
| 2 | 校验器 R1–R3 + VSR stage（首次激活分辨率行为）+ 码率预估 | 1080p→4K 流肉眼优于 bilinear，无泄漏 |
| 3 | temporal 契约 + LSFG stage + 元数据合成器 + R4/R7/R8 | 60→120 端到端；生成帧元数据正确；IDR 后快速恢复 |
| 4 | AFMF stage（MIT，首个可随包分发的 backend）+ vendor 分派 | AMD 主机端到端 |
| 5 | WebUI 链编辑器 + `/api/runtime/postprocess` + 文档 | CI 绿 |

依赖：Phase 1/2 可并行（不同维度），Phase 0 是共同前置；VSR 是更便宜的第一个落地。

## 10. 风险与未决问题

- **进程内崩溃**：第三方 DLL 崩溃带走 Sunshine（§2.4 边界），bypass 无法兜底；
  文档明示，长期可评估 out-of-proc 采样代价。
- **多 temporal 组合**：技术上允许（R4 警告），画质与延迟无保底；是否 v1 直接
  上限 1 个 temporal 段（配置可解禁）——待定。
- **码率预估精度**：×2 是上界估计，真实码率与内容相关；UI 用"预估"措辞。
- **测试矩阵**：链执行器需域图全边覆盖测试 + 三 backend 真机矩阵
  （NV host × {VSR,HDR,LSFG}、AMD host × {AFMF,LSFG}）。
- **VSR API 确认**：RTX Video SDK zip 尚未下载（NVIDIA 登录），Phase 0 首项。
