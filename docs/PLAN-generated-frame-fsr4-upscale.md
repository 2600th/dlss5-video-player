# 落地方案：生成帧接入 FSR4 超分（消除插帧时的隔帧画质跳变）

> 状态：**REV 2（已自审）**。本文档只描述改法与验证方法，未改动任何代码。
> 目标读者：校验者（需要逐条对照源码事实）、实施者。
>
> **REV 2 修订记录（自审发现，校验时重点看）**：
> - R6 撤销/降级：原方案称"与现有 resize 重建路径同步"——**该路径不存在**。`CreateUpscalePresentResources` 仅在 `Initialize` 调用一次（`D3D12Renderer.cpp:227`），output rung 在渲染器生命周期内固定，现有 `m_upscaleStretch` 也不随窗口 resize 重建（窗口 resize 只动 swapchain，见 `ResizeSwapchain`）。新资源与现有资源同生命周期，**无额外 resize 风险**。
> - reset 策略明确：生成帧上下文**恒传 `reset=true`**（原方案留了"必要时恒 reset"的退路，自审后转正为推荐）。理由：生成帧是插值产物，无真实时域连续性，累积历史无物理意义；且省掉与 `:1484` latch 点的联动维护。R4 随之消除。
> - V3 证据力修正：状态栏引擎名来自上下文#1（`PresentUpscaleEngineName` 只读 `m_fsrUpscaleActive`/`m_easuActive`），**不能证明生成帧走了上下文#2**。改为查新增日志行。
> - 失败 latch 补齐：新增 `m_fgUpscaleFailed`，失败一次永久回退（与现有 FSR `m_failed` 模式对称）。
> - 状态跟踪成员补齐：`m_fgLinearColorState`、`m_fgUpscaleSharpState` 初始状态与 barrier 落实。
> - R1"惰性创建"缓解标注风险：运行期创建重上下文有 island 的 40 ms device-removal 前科（`CreateFrameGenResources` 注释 `:2254-2264`），惰性创建需先验证，默认 eager。

---

## 0. 一句话目标

现状开插帧时，cadence 的**源帧走 FSR4**、**生成帧走 present 链普通拉伸**，导致每隔一帧清晰度跳变。
本方案让**生成帧也过 FSR4**，使两帧同引擎、同画质。

---

## 1. 现状事实基线（已逐条核实，校验时先复核这几条）

| # | 事实 | 出处 |
|---|---|---|
| F1 | 源帧在插帧 present 里**确实**采样超分结果 `UpscaleSRV` | `D3D12Renderer.cpp:2904-2906` |
| F2 | 生成帧采样 `GeneratedFrameClampedSRV`（render 分辨率 BGRA8），不经超分 | `D3D12Renderer.cpp:2866-2877` |
| F3 | 超分 pass `RecordUpscalePresent` 在 island 分支**之前**执行，FSR4 每帧都真跑了 | `D3D12Renderer.cpp:1757` vs `:1782` |
| F4 | 超分 target 仅 1 组：`m_upscaleStretch` + `m_upscaleSharp`，均为 `m_outputW×H` **FP16** | `D3D12Renderer.cpp:2032-2088` |
| F5 | island 桥接纹理为 `m_renderW×H` / `B8G8R8A8_UNORM` / `HEAP_FLAG_SHARED` | `D3D12Renderer.cpp:2224-2233` |
| F6 | 生成帧的实际**读回**通过 `_SRGB` 视图，采样器自动解码 | `D3D12Renderer.cpp:2707-2708` |
| F7 | `ffxCreateContext` 为全局函数、`m_context` 为**实例成员** → 可建第二上下文 | `FsrUpscaleBackend.cpp:257`、`FsrUpscaleBackend.h:82` |
| F8 | guides（depth/motion/exposure）是**每实例私有**，且必须在上下文存活前上传 | `FsrUpscaleBackend.cpp:270`、`FsrUpscaleBackend.h:36-40` |
| F9 | FSR4 输入/输出格式在 `Initialize` 时固定为成员 `m_colorFormat`/`m_outputFormat` | `FsrUpscaleBackend.h:54-56, 91-92` |
| F10 | FSR4 有**时域累积**，`dispatch.reset` 控制历史清零 | `FsrUpscaleBackend.cpp:434` |
| F11 | 源帧上下文的 reset 走 **latch**：`if(temporalReset)m_upsResetPending=true;`（`RecordAndPresentFrame` 头部统一分发点） | `D3D12Renderer.cpp:1484` |
| F12 | `CreateUpscalePresentResources` **仅在 `Initialize` 调用一次**，output rung 生命周期内固定，不随窗口 resize 重建 | `D3D12Renderer.cpp:227` |
| F13 | 运行期创建重上下文有前科：island 在 Initialize 期创建会引发 ~40 ms 后 device removal，故 island 延迟到 `EngageFrameGenIsland` | `D3D12Renderer.cpp:2254-2264` 注释 |
| F14 | `m_srvHeap` 容量 = `SRVCount`（加槽自动扩容） | `D3D12Renderer.cpp:341` |

**术语澄清（重要）**：F6 说明生成帧虽创建为 `_UNORM`，但 present 采样走 `_SRGB` 视图 → 采样器解码为线性。本方案新增的线性化 pass 必须**输出 FP16 线性光**，不能重复解码。

---

## 2. 方案总览

### 2.1 采用的管线顺序

```
解码帧 → neural render(源尺寸) → 神经帧
   → [island 插帧 @ render 分辨率]        ← guides 天然正确，保持现状
   → [线性化: 生成帧 BGRA8 → FP16 @ render]
   → [FSR4 上下文#2: FP16@render → FP16@output]   ← 新增
   → present（生成帧与源帧同引擎、同画质）
```

**neural render 保持在最前**：其产物是"源尺寸"帧（`OfflineNeuralRenderer.h:297-298` 明确 `outputWidth/Height` 等于源尺寸），扮演源帧生产者角色，**不能**放在超分之后。

### 2.2 为什么必须用「第二个 FSR4 上下文」而不是复用现有上下文

FSR4 是**时域**上采样器。若源帧与生成帧交替喂入同一上下文（`m_fsrUpscale`），其历史累积会看到"生成帧→源帧"交替序列，产生**闪烁/鬼影**——比现状的画质不一致更糟。

依据 F7：`ffxCreateContext` 全局、`m_context` 实例级 → 建第二上下文成本低，且**时域历史天然隔离**。
依据 F8：第二上下文必须**自建一套 guides**（不能共享第一套），因为 guides 上传发生在上下文创建前，上传后 Map staging 会 TDR。

---

## 3. 详细改动清单

### 改动 1：`FrameGenColorConvertShader.h` — 新增反向 pass

在现有 `frame_gen_convert` 命名空间旁新增（不改动现有 `kHlsl`）：

```
namespace frame_gen_linearize {
  constexpr char kHlsl[] = R"hlsl(
  Texture2D<float4> Src : register(t0);   // _SRGB 视图，硬件解码已发生
  RWTexture2D<float4> Dst : register(u0);  // FP16 线性光 @ render 尺寸
  cbuffer Params : register(b0) { uint gW; uint gH; };
  [numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
      if (id.x >= gW || id.y >= gH) return;
      Dst[int2(id.xy)] = float4(Src[int2(id.xy)].rgb, 1.0f);
  }
  )hlsl";
  constexpr uint32_t kThreadGroup = 8;
}
```

**关键点**：`Src` 必须绑定到 `_SRGB` 描述符（F6），解码由采样器完成，shader 内**不再做 sRGB→线性**，否则二次解码导致画面偏暗 1/2.2。

### 改动 2：`D3D12Renderer.h` — 新增资源与描述符槽

新增 SRV 槽（在 `GeneratedFrameClampedSRV` 之后）：

```cpp
static constexpr uint32_t GeneratedUpscaledSRV = GeneratedFrameClampedSRV + 1;
static constexpr uint32_t SRVCount = GeneratedUpscaledSRV + 1;
```

新增成员资源：

| 成员 | 尺寸 | 格式 | 用途 |
|---|---|---|---|
| `m_fgLinearColor` | `m_renderW×H` | `R16G16B16A16_FLOAT` + UAV | 线性化输出，FSR4 #2 的输入 |
| `m_fgUpscaleStretch` | `m_outputW×H` | `R16G16B16A16_FLOAT` + UAV\|RT | FSR4 #2 输出 |
| `m_fgUpscaleSharp` | `m_outputW×H` | `R16G16B16A16_FLOAT` + RT | CAS 锐化输出，present 采样 |
| `m_fsrUpscaleGenerated` | — | `unique_ptr<FsrUpscaleBackend>` | 第二 FSR4 上下文 |
| `m_fgLinearColorState` | — | `D3D12_RESOURCE_STATES` | 线性化目标状态跟踪，初始 `UAV` |
| `m_fgUpscaleSharpState` | — | `D3D12_RESOURCE_STATES` | 锐化目标状态跟踪，初始 `PIXEL_SHADER_RESOURCE` |
| `m_fgUpscaledReady` | — | `bool`（每帧） | 本帧生成帧超分是否成功 |
| `m_fgUpscaleFailed` | — | `bool`（latch） | 失败一次永久回退（对称现有 `m_failed`） |

新增 descriptor heap：
- `m_fgLinearHeap`（shader-visible，4 槽）：槽0 SRV `m_fgFfxClamped`(_SRGB)、槽1 SRV `m_fgFfxOutput`(_SRGB)、槽2 UAV `m_fgLinearColor`（线性化 pass 用，输入二选一按 `m_fgClampRan`）
- `m_fgUpscaleHeap`（shader-visible，1 槽）：SRV `m_fgUpscaleStretch`（CAS 锐化读，**不能复用 `m_upscaleSrvHeap`**——其唯一槽的 view 固定指向源帧的 `m_upscaleStretch`）
- 新 RTV：`m_fgUpscaleSharp` 加进现有 `m_upscaleRtvHeap`？**不行**——该 heap 固定 2 槽（NumDescriptors=2，`:2065`）。需新建小 RTV heap 或扩容；建议新建 `m_fgUpscaleRtvHeap`（1 槽）。

**reset 策略（REV 2 明确）**：上下文#2 **恒传 `reset=true`**，不引入 `m_fgUpscaleResetPending`，不挂 `:1484` latch 点。理由：生成帧为插值产物，无真实时域连续性，历史累积无物理意义；恒 reset 后 R4 时域风险消除，且省掉与源帧 reset 联动的维护成本。FSR4 恒 reset 退化为纯空间重建，与视频场景（零 jitter、零 motion，见 `FsrUpscaleBackend.h:21-35`）本就受限的时域收益一致，画质损失预期可忽略——**此点需 V6 实测确认**。

### 改动 3：`D3D12Renderer.h/.cpp` — 新增记录方法

```cpp
// 在 island 生成之后、present 之前调用
bool RecordFrameGenUpscale(ID3D12GraphicsCommandList* cmd);
```

实现要点：

0. **入口守卫**：`if (m_fgUpscaleFailed || !m_fsrUpscaleGenerated) return false;`——失败 latch 一次，之后每帧直接回退，不再浪费 dispatch。
1. **线性化**：输入按 `m_fgClampRan && m_fgFfxClamped ? m_fgFfxClamped : m_fgFfxOutput` 二选一，SRV 必须是 `_SRGB` 格式视图（F6：解码由采样器完成）；输入 barrier 从各自当前状态（clamp 路径下 `m_fgFfxClamped` 在 clamp 结束时为 UAV 态，见 `:2876-2877`；raw 路径下 `m_fgFfxOutput` 为 `m_fgFfxOutputState`）转到 `NON_PIXEL_SHADER_RESOURCE`；输出 `m_fgLinearColor` 从 `m_fgLinearColorState` 转 UAV。dispatch 尺寸 `m_renderW/H`。
2. **FSR4 #2 dispatch**：`m_fgLinearColor`（NON_PIXEL/COMPUTE_READ）→ `m_fgUpscaleStretch`（UAV），**`reset` 恒 `true`**（见改动 2 的 reset 策略），`frameTimeMs` 传 `m_fgFrameTimeMs`（与 island 同值）。dispatch 后 `m_fgLinearColorState` 记为 NON_PIXEL。
3. **CAS 锐化**：绑 `m_fgUpscaleHeap`（SRV→`m_fgUpscaleStretch`）+ `m_fgUpscaleRtvHeap`（RTV→`m_fgUpscaleSharp`），PSO/root sig/常量复用现有 `m_psoUpscaleSharp` + `m_upscaleRootSig`（8 常量与源帧 CAS 相同：renderW/H、outputW/H、sharpness）。barrier：`m_fgUpscaleStretch` UAV→PIXEL，`m_fgUpscaleSharp` →RT，draw 后 →PIXEL 并更新 `m_fgUpscaleSharpState`。
4. **首帧日志**（V3 的证据）：`m_fgUpscaledReported` latch，首次成功打 `LOG("Playback frame generation: generated frames are upscaled by FSR 4 (" << m_renderW << "x" << m_renderH << " -> " << m_outputW << "x" << m_outputH << ").")`。
5. **失败处理**：线性化或 dispatch 失败 → `m_fgUpscaleFailed=true` + 日志，`return false`；调用方回退到现状行为（生成帧走普通拉伸），**源帧路径完全不受影响**。注意：FSR4 `Dispatch` 失败时后端自身已 latch `m_failed`（`FsrUpscaleBackend.cpp:449`），外层 latch 防的是线性化/CAS 段失败。
6. **heap 卫生**：每段 pass 前显式 `SetDescriptorHeaps`（线性化→`m_fgLinearHeap`；FSR4→runtime 自管；CAS→`m_fgUpscaleHeap`；present→`m_srvHeap`），与现有 `RecordUpscalePresent` 的模式一致（`:3235`、`:3268` 都在 draw 前重设 heap）。

### 改动 4：`CreateUpscalePresentResources()` — 创建新资源

在现有 FSR4 初始化块（`:2143-2171`）之后，追加第二上下文初始化：

```cpp
if (m_fsrUpscaleActive) {
    auto gen = std::make_unique<FsrUpscaleBackend>();
    if (gen->Initialize(m_device.Get(), m_renderW, m_renderH,
                        m_outputW, m_outputH,
                        DXGI_FORMAT_R16G16B16A16_FLOAT,
                        DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        m_fsrUpscaleGenerated = std::move(gen);
        m_fgUpscaleResetPending = true;
    } else {
        LOG("Playback frame generation: the generated-frame FSR 4 context would "
            "not start (...); generated frames keep the compositor stretch.");
    }
}
```

**注意**：格式与上下文 #1 完全相同（F9），因此 `Initialize` 签名无需改动；差异只在**输入资源不同**（`m_fgLinearColor` vs `m_dlssColor`），而 `Dispatch` 的 color 是**参数**，所以同一后端类可复用，无需改 `FsrUpscaleBackend`。

### 改动 5：`RecordFrameGenPresentDraw()` — 生成帧改采超分结果

在 `generated` 分支（`:2864`）的最前面插入：

```cpp
if (generated) {
    if (m_fgUpscaledReady && m_fgUpscaleSharp) {
        Barrier(cmd, m_fgUpscaleSharp.Get(), m_fgUpscaleSharpState,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->SetGraphicsRootDescriptorTable(RootView, SRVGPU(GeneratedUpscaledSRV));
        cmd->DrawInstanced(3, 1, 0, 0);
        Barrier(cmd, m_fgUpscaleSharp.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                m_fgUpscaleSharpState);
        m_fgUpscaleSharpState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    } else if (/* 现有 clamp 分支 */) { ... }
```

即**新增最优先分支**，失败时自动落回现有逻辑。采样后状态还原为 PIXEL（下帧 CAS 会有 PIXEL→RT 的 barrier 兜底），跟踪成员同步更新。

**debug view 路径说明（REV 2 补）**：`RecordFrameGenPresentDraw` 的第三个调用点（debug view / reset 帧，`:1851`）**不经过** `RecordFrameGenUpscale`（该函数只在 cadence 分支调用，见改动 6），此时 `m_fgUpscaledReady` 为默认 false → 自动走现有分支，debug view 行为与现状完全一致。校验时确认 `m_fgUpscaledReady` 的置位点仅此一处即可。

### 改动 6：调用点 — 在 present 前触发

在 `RecordAndPresentFrame` 的 cadence 分支（`:1815-1846`）中，`RecordFrameGenClamp`（`:1818`）之后、`RecordFrameGenPresentDraw(cmd,slot,true)`（`:1834`）之前：

```cpp
m_fgUpscaledReady = m_playbackFrameGen && showGenerated && !m_fgUpscaleFailed &&
                    RecordFrameGenUpscale(cmd);
```

**位置依据**：`cmd` 在 `:1813` 已 reset、`:1814` 已绑 `m_srvHeap`，island 等待已结束（生成结果已落盘），clamp 结果已就绪——是唯一正确的插入窗口。`m_fgUpscaledReady` 置位点**仅此一处**（见改动 5 的 debug view 说明）。

### 改动 7：释放路径

`Shutdown`/析构中 `Reset()` 所有新增成员：`m_fgLinearColor`、`m_fgUpscaleStretch`、`m_fgUpscaleSharp`、`m_fgLinearHeap`、`m_fsrUpscaleGenerated`。

---

## 4. 风险清单（按严重度）

| # | 风险 | 严重度 | 缓解 |
|---|---|---|---|
| R1 | **第二个 FSR4 上下文的显存与驱动开销**：FSR4 有 sizeable 的内部资源 | 中 | 两上下文常驻（eager 创建）。~~惰性创建~~ **需先验证**：运行期创建重上下文有 island 的 40 ms device-removal 前科（F13），`CreateGuides` 用后端自有 command list 上传，现有先例都在 Initialize 前段做。惰性方案在校验通过前不作为缓解 |
| R2 | **guides 双份**：`CreateGuides` 为每实例建 depth/motion/exposure 三张常量纹理 | 低 | 常量 1×1，开销可忽略 |
| R3 | **性能**：每源帧多一次 FSR4 dispatch（render→output 尺寸） | 中 | 见 §5 实测；不达标则退回纯 shader 版 |
| R4 | ~~生成帧时域历史不稳~~ **已通过"恒 reset=true"消除**（改动 2） | ~~中~~ 低 | 恒 reset 后上下文#2 无历史累积；V6 仍需实测确认无鬼影（FSR4 纯空间模式的行为） |
| R5 | **颜色域二次解码**（画面偏暗） | 高（易犯） | F6 的 `_SRGB` 视图；shader 内不再解码 |
| R6 | ~~resize 未重建~~ **撤销**：`CreateUpscalePresentResources` 仅 Initialize 调用一次（F12），output rung 生命周期内固定，现有 `m_upscaleStretch` 同样不随窗口 resize 重建 | ~~中~~ 无 | 新资源与现有资源同生命周期，无额外风险；V7 弱化为"确认行为不劣于现状" |
| R7 | 我**无法在沙箱编译验证**（`build-fsr4.bat` 需本机 vcvars 环境） | — | 见 §6 构建与验证步骤 |
| R8 | **`m_fgFfxOutput`/`m_fgFfxClamped` 的状态机交互**：线性化 pass 新增了对这两个资源的读取，其 barrier 序必须与 clamp pass（`:2876-2877` 还原 UAV）和 present 采样（`:2884-2889`）的既有状态流转一致 | 中 | 改动 3 已写明各 barrier 落点；校验者重点核对 `m_fgFfxClampedState`/`m_fgFfxOutputState` 在新 pass 前后的值 |

---

## 5. 性能预期（需实测校验）

以当前 800×432 → 2560×1382（16× 放大）为例：

- FSR4 dispatch 像素量：render(345K) → output(3.5M)，**成本主要落在 output 尺寸的 3.5M 像素重建**（render 尺寸读取为次要项）
- 新增：每源帧多一次同量级 dispatch → **超分总耗时约翻倍**
- 预期：插帧开启且 FSR4 生效时，GPU 帧时间增加约一次 FSR4 dispatch 的耗时

**校验方法**：对比开/关插帧时的 `Playback health: presented=` 与 GPU 计时日志；若帧率跌破可接受阈值，按 R3 退回纯 shader 方案。

---

## 6. 构建与验证步骤（供校验者执行）

### 构建
```powershell
cd F:\dlss5-video\dlss5-video-amd
.\build-fsr4.bat
```
> 沙箱内无法执行（需手动 vcvars64 环境），必须在本机跑。

### 验证清单

| # | 步骤 | 期望 |
|---|---|---|
| V1 | 查看构建是否通过（`/WX /W4` 严格） | 0 error |
| V2 | 启动播放，日志搜 `generated frames are upscaled by FSR 4`（改动 3 第 4 条新增） | 第二上下文工作 |
| V3 | 开超分 + 开插帧，观察生成帧清晰度 | 与源帧一致，无隔帧跳变（**主验证项**；注意：状态栏引擎名只反映上下文#1，不作为本项证据） |
| V4 | **A/B 画质对比**：开/关插帧，源帧清晰度应基本一致 | 无隔帧跳变 |
| V5 | 检查亮度 | 与改动前一致（**验证 R5**） |
| V6 | 观察鬼影/闪烁 | 无（验证 R4：恒 reset 下 FSR4 纯空间模式的行为） |
| V7 | 窗口 resize 后播放 | 行为不劣于现状（R6 已撤销，此项仅回归确认） |
| V8 | 记录开/关插帧的帧率 | 评估 R3 是否可接受 |
| V9 | 插帧 + 超分 + neural render 三开 | 三者均工作，无 device removal |
| V10 | 关掉超分开关（仅插帧） | 生成帧回退普通拉伸（`UpscalePresentActive()` 假 → 上下文#2 不工作，确认守卫路径） |
| V11 | 强制 clamp 不可用（若可构造） | raw `m_fgFfxOutput` 路径的线性化正常 |

---

## 7. 与其他方案的对比（决策留痕）

| 方案 | 画质一致性 | 改动量 | 性能代价 | 风险 |
|---|---|---|---|---|
| **本方案（FSR4 独立上下文）** | ✅ 完全一致 | 中 | ~1× 额外 FSR4 dispatch | R4 时域、R6 resize |
| 纯 shader 版（EASU/CAS 给生成帧） | ⚠️ 档位差（FSR4 vs FSR1） | 小 | 低 | 低 |
| B（先超分后插帧，输出分辨率插帧） | ✅ 一致且插帧质量最高 | **大** | 双 16× 像素 | 高（bridge 重建、时序） |
| 现状 | ❌ 隔帧跳变 | — | — | — |

---

## 8. 校验者重点复核项（REV 2 更新）

1. **R5（最高风险）**：确认生成帧的 `_SRGB` SRV 视图与线性化 shader 不重复解码。
2. **改动 2 的 heap 方案**：`m_fgLinearHeap` 4 槽（2 SRV + 1 UAV + 预留）与 root sig 的 descriptor table 布局是否匹配；`m_fgUpscaleRtvHeap` 新建的必要性（现有 `m_upscaleRtvHeap` 固定 2 槽，`:2065`）。
3. **R8（新增）**：`m_fgFfxClampedState`/`m_fgFfxOutputState` 在线性化 pass 前后的状态流转，与 clamp pass（`:2876-2877`）、present 采样（`:2884-2889`）的既有 barrier 序是否自洽。
4. **恒 reset 决策**：生成帧放弃时域累积是否可接受（V6 实测；备选方案为挂 `:1484` latch 点与源帧联动）。
5. **R1 的 eager 创建**：两上下文都建在 Initialize 前段，确认无 F13 类的运行期创建问题（本方案默认 eager，无惰性）。
6. **§1 基线事实**：逐条复核 F1–F14，尤其 F6（_SRGB 解码）、F7（第二上下文可行性）、F12（rung 固定）、F13（创建时机前科）。

---

*文档版本：REV 2（2026-10-17 自审修订）| 状态：待用户校验，未改代码*