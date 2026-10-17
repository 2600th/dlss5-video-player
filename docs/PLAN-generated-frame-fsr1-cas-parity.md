# PLAN: 生成帧 FSR1 / CAS 档同引擎改造（扩展 generated-frame upscale 到全降级链）

状态：**已实施（2026-10-17，BUILD_EXIT=0）** — 落地于 `D3D12Renderer.cpp` / `D3D12Renderer.h`，与 FSR4 专项同分支 `on_amd_framegen2_fsr4` 提交。运行时验证 V1 通过（编译）；V2–V7 需在带 GPU 的本机执行 `build-fsr4.bat` 验证（沙箱不可做）。

本计划扩展 [`PLAN-generated-frame-fsr4-upscale.md`](./PLAN-generated-frame-fsr4-upscale.md) 的成果——把"生成帧走独立 FSR4 上下文"推广到 **FSR1(EASU) 与 CAS(Catmull-Rom) 两档**，使 cadence 两帧在全部三档（fsr4 / fsr1 / cas）都同引擎同画质。源帧的三档降级链 `fsr4 → fsr1 → cas` **不被改动**，本计划只是在生成帧侧镜像同一选择。

---

## 动机

- 已落地的 FSR4 专项使 **FSR4 档**下源帧 = FSR4、生成帧 = FSR4 #2，消除了该档隔帧跳变。
- 但 **FSR1 / CAS 档**下，生成帧仍走合成器普通拉伸（改动前即如此），cadence 两帧仍有轻微画质差。
- 目标：生成帧在每一档都跟随源帧实际使用的引擎，做到三档全对等。

---

## 已确立事实（沿用 FSR4 计划 + 本次核对）

- **F1.** 源帧超分三段：`RecordUpscalePresent` 中 `useFsr`(FSR4) → `if (!stretched && m_easuActive)`(EASU) → else(Catmull-Rom)；末段统一 CAS 锐化。三档共用 `m_upscaleRootSig` + 8 常量（render/output 尺寸 + sharpness；EASU 用 `m_easuCon`）。
- **F2.** 源帧输入 `m_dlssColor` 为**线性 FP16**（`R16G16B16A16_FLOAT`，渲染器的线性光输出），其 SRV 在 `m_srvHeap` 槽 4（`SRVGPU(4)`）。stretch 读它写 `m_upscaleStretch`，CAS 读 `m_upscaleStretch` 写 `m_upscaleSharp`。
- **F3.** 生成帧（island 插值）`m_fgFfxOutput` 为 **`B8G8R8A8_UNORM_SRGB`**（sRGB 编码 BGRA8），与源帧不在同一色彩域——必须经 `frame_gen_linearize` 手动 `SrgbToLinear` 转成 FP16 linear（`m_fgLinearColor`），才能喂给 FSR4 / EASU / Catmull-Rom（它们都按线性 FP16 输入，同 `m_dlssColor`）。
- **F4.** 生成帧专用资源/heap 已存在（FSR4 专项创建，但当前门控在 `m_fsrUpscaleGenerated`）：`m_fgLinearColor`(render-res FP16 UAV)、`m_fgUpscaleStretch`/`m_fgUpscaleSharp`(output-res FP16，RT+UAV)、`m_fgLinearHeap`(4 槽，linearize compute 用)、`m_fgUpscaleHeap`(1 SRV：`m_fgUpscaleStretch`)、`m_fgUpscaleRtvHeap`(1 RTV：`m_fgUpscaleSharp`)、`m_fgLinearRootSig`/`m_psoFgLinear`。
- **F5.** `m_upscaleRootSig` / `m_psoUpscaleEasu` / `m_psoUpscaleStretch` / `m_psoUpscaleSharp` 在源超分存在时（`m_upscaleOffered`）必然已建，生成帧可直接复用，**无需新建 PSO**。

---

## 改动

### Change A — 放宽生成帧资源创建门控

- 位置：`CreateFrameGenResources` 中 `if (m_fsrUpscaleGenerated)`（约 2752，FSR4 专项创建块）。
- 改为：`if (m_upscaleOffered)`（源超分存在即可，不限 FSR4）。这样 EASU / CAS 档也有 `m_fgLinearColor` / `m_fgUpscaleStretch` / `m_fgUpscaleSharp` / linearize PSO / heaps。
- **不变**：FSR4 #2 上下文 `m_fsrUpscaleGenerated` 的创建仍门控于 FSR4（`CreateUpscalePresentResources` 内 `if (m_fsrUpscaleActive)`，约 2195，不改）。

### Change B — 扩展两个 heap 的槽位

- `m_fgUpscaleHeap`：1 → **2** 个 SRV。slot0 = `m_fgLinearColor`（stretch 输入，`R16G16B16A16_FLOAT`）；slot1 = `m_fgUpscaleStretch`（CAS 输入，沿用原 slot）。
- `m_fgUpscaleRtvHeap`：1 → **2** 个 RTV。slot0 = `m_fgUpscaleStretch`（stretch 输出）；slot1 = `m_fgUpscaleSharp`（CAS 输出，沿用原 slot）。
- 位置：约 2895–2924 的 CAS heap / CAS RTV heap 创建处。新建 SRV / RTV 视图指向新增资源。
- 说明：合并到这两个 heap 比新建独立 heap 更简洁，且完全自包含（不依赖 `m_srvHeap` 槽位算术），与 FSR4 专项一致。

### Change C — 改写 `RecordFrameGenUpscale` 为"分档 stretch + 统一 CAS"

- 位置：`D3D12Renderer.cpp` 约 3084–3191。
- 步骤：
  1. **线性化（全档通用）**：`frame_gen_linearize` compute 将 `m_fgFfxClamped`（或 raw `m_fgFfxOutput`）的 sRGB BGRA8 → `m_fgLinearColor` FP16 linear。障栅 `m_fgLinearColor` UAV →（依档 NOP / P SRV）。
  2. **分档 stretch**（镜像源帧选择，读 `m_fgLinearColor` 写 `m_fgUpscaleStretch`）：
     - 若 `m_fsrUpscaleActive && m_fsrUpscaleGenerated` → **FSR4 #2** dispatch（UAV 写，`reset=true`，隔离时域历史防鬼影）——沿用现有。
     - 否则若 `m_easuActive` → **EASU** draw：heap = `m_fgUpscaleHeap`，root sig = `m_upscaleRootSig`，PSO = `m_psoUpscaleEasu`，输入 SRV = `m_fgUpscaleHeap` slot0（`m_fgLinearColor`），输出 RTV = `m_fgUpscaleRtvHeap` slot0，常量 = `m_easuCon`。障栅 `m_fgUpscaleStretch` RT → PSRV。
     - 否则 → **Catmull-Rom** draw：同上，PSO = `m_psoUpscaleStretch`，常量 = stretch 常量（render/output 尺寸 + sharpness）。障栅同上。
  3. **统一 CAS 锐化（全档通用）**：`m_psoUpscaleSharp` draw，输入 SRV = `m_fgUpscaleHeap` slot1（`m_fgUpscaleStretch`），输出 RTV = `m_fgUpscaleRtvHeap` slot1（`m_fgUpscaleSharp`），常量 = sharpen 常量。障栅 `m_fgUpscaleSharp` PIXEL → RT → PIXEL。
  4. **日志**：首成功按实际档输出（`generated frames are upscaled by FSR 4 / FSR1 EASU / Catmull-Rom ...`）。
- **档间状态机自洽（关键）**：`m_fgLinearColor` 在线性化后是 UAV，stretch 消费者要求不同态——FSR4 #2(compute) 要 `NON_PIXEL`，EASU / Catmull-Rom(pixel draw) 要 `PIXEL`。按选中档 transition 后再喂入；stretch 输出 `m_fgUpscaleStretch` 在 FSR4 下是 UAV、在 EASU / Catmull-Rom 下是 RTV，stretch 后统一 RT/UAV → PSRV 供 CAS。
- **FSR4 #2 失败的可选 fall-through（对齐源帧稳健性）**：若 FSR4 #2 dispatch 返回 false，不永久 latch `m_fgUpscaleFailed`，而是置 `stretched = false` 并落入 EASU / Catmull-Rom 分支（与源帧 `if (!stretched && m_easuActive)` 同构）；仅资源 / PSO 创建失败才 latch。推荐采用此对齐设计；若求稳也可保留当前"永久 latch"行为。

### Change D — 放宽 `RecordFrameGenUpscale` 守卫

- 约 3089 守卫：去掉对 `m_fsrUpscaleGenerated` 的硬依赖，改为要求生成帧资源存在：
  `!m_fgLinearColor || !m_fgUpscaleStretch || !m_fgUpscaleSharp || !m_psoFgLinear || !m_fgLinearHeap || !m_fgUpscaleHeap || !m_fgUpscaleRtvHeap || !m_upscaleRootSig || !m_psoUpscaleSharp` 即返回 false（生成帧退回合成器拉伸，原行为）。
- `RecordAndPresentFrame` 中 `m_fgUpscaledReady = ... && RecordFrameGenUpscale(cmd)`（约 1827）**无需改**——`RecordFrameGenUpscale` 任一档成功即返回 true。
- `RecordFrameGenPresentDraw` 的 `if (m_fgUpscaledReady && m_fgUpscaleSharp)`（约 3223）采样 `m_fgUpscaleSharp`——全档通用，**无需改**。

### Change E — 头文件声明

- `D3D12Renderer.h`：为 `m_fgUpscaleHeap` / `m_fgUpscaleRtvHeap` 注释更新为 2 槽；如有必要新增"当前档"标志用于日志（也可仅靠一次日志省略）。本计划**不新增成员**——`m_upscaleRootSig` / `m_psoUpscaleEasu` / `m_psoUpscaleStretch` 已是类成员，直接复用。

### Change F — 释放路径

- `ReleaseFrameGenBridge`（约 3363–3365）已 reset `m_fgLinearHeap` / `m_fgUpscaleHeap` / `m_fgUpscaleRtvHeap` 及资源——**无需改**（heap 数量不变，仅槽内槽位增多）。

---

## 风险

- **R1.** 档间状态机（`m_fgLinearColor` 的 NOP / PIXEL、`m_fgUpscaleStretch` 的 UAV / RTV）若 transition 错会导致 TDR / 设备移除。务必按 Change C 的障栅表逐档核对；可先用 `m_fgUpscaleFailed` latch 快速回退验证。
- **R2.** 复用 `m_upscaleRootSig`（1 SRV 表 + 8 常量）对 EASU / Catmull-Rom 完全适用（源帧即如此），但需确认生成帧 draw 的 SRV 表绑定落在 `m_fgUpscaleHeap` 的 slot0（而非源帧的 `SRVGPU(4)`）——本计划已自包含 heap，不触碰 `m_srvHeap` 槽位，规避该坑。
- **R3.** 线性化是逐帧 compute，EASU / CAS 档也每帧多一次 dispatch + 两次 draw。性能开销与 FSR4 档同级，可接受；低端 GPU 可观测帧时间，必要时用 `m_fgFrameTimeMs` 监控。
- **R4.** 若源帧在该帧因 FSR4 失败而中途降级（罕见），本计划 Change C 的 fall-through 设计可让生成帧同步降级；若保留"永久 latch"简化版，则那一帧及之后生成帧退回合成器拉伸（与源帧短暂不同步，但稳态仍一致）。

---

## 验证

- **V1.** 编译：`cmake --build build-ninja --target DLSSVideoPlayer` 在 `/WX /W4` 下 `BUILD_EXIT=0`（沙箱可做）。
- **V2.** **FSR4 档**（带 RX 7000/9000 + FSR4 runtime 的本机）：日志出现 `generated frames are upscaled by FSR 4`，隔帧跳变消除，亮度一致（验 sRGB 纠正），无鬼影（验恒 reset）。
- **V3.** **FSR1 档**：在无法跑 FSR4 的 GPU（如 RX 6000 / 核显）或强制 `m_fsrUpscaleActive=false` 的环境下，日志出现 `generated frames are upscaled by FSR1 EASU`，源帧与生成帧同引擎，隔帧跳变消除。
- **V4.** **CAS 档**：在 EASU PSO 编译失败（极罕见）或同样强制环境下，日志出现 `generated frames are upscaled by Catmull-Rom`，两帧同引擎。
- **V5.** 三项档位下分别抓取源帧与生成帧像素对比（同静态画面），确认无隔帧清晰度 / 亮度差异。
- **V6.** 资源 / PSO 创建失败时 `m_fgUpscaleFailed` latch，生成帧安全退回合成器拉伸，无 TDR。
- **V7.** 档位切换（运行时改输出分辨率触发 rung 重建 / 中途 FSR4 失败）后，生成帧跟随新档，无残留旧态。

> 测试提示：FSR4 专项只能在 RX 7000/9000 验证；FSR1 / CAS 档的验证可在不支持 FSR4 的 GPU 上自然触发，或用调试覆盖强制 `m_fsrUpscaleActive=false`（例如临时制造 `ratio > 3.0` 或加开关）来逼出 EASU / Catmull-Rom 路径。

---

## 与 FSR4 计划的关系

- 本计划是 [`PLAN-generated-frame-fsr4-upscale.md`](./PLAN-generated-frame-fsr4-upscale.md) 的**超集扩展**：FSR4 #2 部分（`m_fsrUpscaleGenerated`、恒 reset、sRGB 纠正）全部保留；仅把"生成帧上采样"从"仅 FSR4"推广为"按源帧实际档位选择 FSR4 / EASU / Catmull-Rom + 统一 CAS"，并放宽资源创建门控与 heap 槽位。
- **源帧超分内部实现与 `fsr4 → fsr1 → cas` 降级顺序零改动。**
