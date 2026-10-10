# DLSS 5 Video Player on AMD：LmxxfNrRuntime 接入方案修订版

_核对基线：播放器 `f42cb39` / 0.28.2，本地方案修订日期 2026-10-10。_

当前进度（2026-10-10）：

- 已固定 Lmxxf ABI 头：`third_party/lmxxf/LmxxfNrApi.h`。
- 已加入纯 C++ ABI/能力校验：`src/AmdLmxxfApi.h`。
- 已加入 RDNA4/`gfx1200`/`gfx1201` 与运行目录判定：`src/AmdLmxxfPolicy.h`。
- 已加入运行时 session 调用状态机：`src/AmdLmxxfSession.h`。
- 已加入 Windows 动态加载器：`src/AmdLmxxfRuntime.h/.cpp`。
- 已加入 `AmdLmxxfApiTests`，当前在 Mac 上直接编译通过。
- D3D12 设备探针、资源上传和真实 runtime 预检尚未实现。

## 1. 修订结论

接入 `dlss5-on-amd-9070xt-porting` 的 `LmxxfNrRuntime.dll` 仍然可行，
而且它仍是最合适的 AMD 主后端。

但这不是“只加一个 DLL 加载器”的小改造。按当前 0.28.2 代码重新核对后，
至少涉及时下四块：

1. `NeuralWorker` 的生产神经求值器。
2. `OfflineNeuralRenderer` 的请求、结果和发布门禁。
3. `NeuralWorkerProtocol`、`NeuralCacheManifest`、`NeuralReceipt` 的
   后端中立化。
4. GPU 识别、运行时布局、预检、运行锁和 resident helper 策略。

这样做完后，播放器的解码、时间轴、缓存、对比、导出和分段直播链路可以继续
复用，不需要再写一套 AMD 播放器。

## 2. 当前播放器仍然适合接这个后端

### 2.1 `INeuralFrameEvaluator` 是正确的接缝

`src/OfflineNeuralRenderer.h` 已经定义了：

```cpp
class INeuralFrameEvaluator {
    virtual bool Initialize(...) = 0;
    virtual bool Submit(...) = 0;
    virtual bool FeatureCreated() const = 0;
    virtual uint64_t EvaluationCount() const = 0;
    virtual uint64_t NeuralEvaluations() const = 0;
    virtual void ResetTemporal() = 0;
    virtual NeuralRenderFailure LastFailure() const = 0;
    virtual double LastNeuralGpuMs() const;
    virtual uint64_t PeakLocalVideoMemoryMiB() const;
};
```

`LmxxfNrRuntime` 可以包装为一个新的 evaluator：

```text
OfflineNeuralRenderer
  -> AmdLmxxfEvaluatorAdapter
       -> LmxxfNrRuntime.dll
       -> private_output -> readback BGRA/NV12
```

不需要修改任务级的 `RunJob` 主循环，也不需要在主播放器里加载 runtime。

### 2.2 D3D12 所有权一致

`LmxxfNrRuntime` 要求宿主提供：

- `ID3D12Device*`
- `ID3D12CommandQueue*`
- `ID3D12GraphicsCommandList*`
- 一个颜色 `ID3D12Resource*`

播放器 worker 自己拥有这些对象，不需要第二设备、共享纹理跨界或 ReShade。

### 2.3 新 fork 已把 HDR 源先转成 SDR

0.28.2 在解码阶段对 HDR 源做 tone map，神经模型看到的是 SDR 画面。
这恰好适合 `LmxxfNrRuntime` 当前以 SDR/BGRA 为主的工作域。

第一版仍应明确限制：

```text
AMD 神经输入端：SDR
缓存输出：8-bit BGRA 或 NV12
HDR 输出/比较：后处理，不改变 AMD 模型的输入
```

## 3. 上一版方案漏掉的四个硬约束

### 3.1 生产 evaluator 不是可插拔的

虽然公开了 `INeuralFrameEvaluator`，但默认构造的
`OfflineNeuralRenderer::Retained` 硬编码为：

```text
ProductionSourceAdapter
ProductionEvaluatorAdapter
ProductionEncoderAdapter
SessionEvidence
```

其中 `ProductionEvaluatorAdapter` 直接持有 `D3D12Renderer`，并使用 NGX、
ReShade 日志、feature 18 和 NVIDIA 专用资源。

因此 AMD 不能只靠现有注入构造函数解决。生产路径需要新增：

```text
AmdLmxxfEvaluatorAdapter
AmdLmxxfEvidence
```

并在 `Retained` 内按后端选择。

### 3.2 当前发布门禁把 feature 18 当成“成功”的同义词

`Finish`、`CaptureAttempt::HoldReceiptGate` 和 `NeuralCache` 明确检查：

- `feature18Created`
- `feature18Evaluated`
- `feature18ArmedBeforeCapture`
- `highestObservedEvaluation`
- NGX 后端 evaluation count
- NVIDIA 校准的 `NeuralGpuMsFloor`

Lmxxf 没有 feature 18，也不会写 `ReShade.log`。继续套用这些条件会导致：

- 正常 AMD 帧无法通过证据门禁。
- 或为了绕过门禁而放宽验证，把错误输出写进缓存。

必须改为后端中立的证据模型。

### 3.3 缓存身份和 manifest 当前无法表达 AMD

当前 `NeuralCacheIdentity` 只有：

```text
gpuPath
runtimeDigest
quality
settingsDigest
driverVersion
modelStoreDigest
```

其中：

- `gpuPath` 只识别 NVIDIA 代次。
- `runtimeDigest` 指向 ReShade/NeuroDX 运行目录。
- `modelStoreDigest` 指向 NVIDIA NGX 模型库。
- `quality` 的 pipeline 项写着 `DLAA|armed-inline-interception-v3`。

AMD runtime 需要不同的身份项：

```text
backend = amd-lmxxf
gpuPath = amd-rdna4-gfx1201
runtimeDigest = LmxxfNrRuntime.dll + shaders + HSACO + config
modelStoreDigest = weights digest
quality = 1-pass/fast|exact|style|network-height|fit-large
```

### 3.4 运行目录和预检完全按 ReShade 设计

当前流程固定寻找：

```text
neural-runtime/
  ReShade.ini
  dxgi.dll
  renodx-dlss5.addon64
  nvngx_dlssnr.dll
  NeuralWorker.exe
```

AMD 方案需要独立布局：

```text
amd-neural-runtime/
  LmxxfNrRuntime.dll
  NeuralWorker.exe
  native-game-tiled-assets/
    shaders/
    weights/
    HIP/gfx1200/
    HIP/gfx1201/
    SHA256SUMS
  AmdNeuralRuntime.ini
  amd-runtime-lock.json
```

## 4. 推荐的最终架构

```text
DLSSVideoPlayer.exe
  |
  | NeuralWorkerProtocol v7
  v
NeuralWorker.exe
  |
  +-- 解码 / HDR tone map / 播放时间轴
  +-- Backend selection
  |     |
  |     +-- NvidiaRenoDxBackend
  |     |     D3D12Renderer + NGX + ReShade
  |     |
  |     +-- AmdLmxxfBackend
  |           AmdLmxxfRuntimeHost
  |           AmdLmxxfEvaluatorAdapter
  |           D3D12 upload / HIP handoff / readback
  |
  +-- Encoder
  +-- Cache / receipt / manifest
```

主播放器仍然保持 hook-free，只播放已缓存或分段发布的结果。

## 5. AMD evaluator 的逐帧调用

### 5.1 资源

第一版建议：

```text
Input:
  BGRA8 或运行时接受的等价格式
  一张持久输入纹理，不每帧换指针

Output:
  LmxxfNrJob::private_output
  一张读回资源，按 slot 轮转

Queue:
  与 runtime 创建时使用同一条 D3D12 graphics queue
```

颜色资源指针稳定很重要。Lmxxf runtime 文档明确说明：

> 每帧换颜色资源指针、尺寸、格式或曝光绑定会重建或重绑编解码链，
> 可能要等 GPU。

所以不要做“每帧创建一张 input texture”的简单实现。

### 5.2 提交顺序

每帧严格按：

```text
1. PrepareFrame(info, &job)
2. producer list:
     upload / transition input
     RecordInputs(producer)
3. ExecuteCommandLists(producer)
4. EnqueueHip(session.queue, job)
5. consumer list:
     RecordOutputs(consumer)
     transition private_output -> COPY_SOURCE
     CopyResource(private_output -> readback)
6. ExecuteCommandLists(consumer)
7. Retire(job)
```

保持 2 到 3 帧在飞，不要每帧：

- `Drain`
- 等待整条队列
- 重新创建 session
- 重新加载模块或权重

### 5.3 时序重置

调用 `ResetHistory` 的时机：

- `FirstFrame`
- `Seek`
- `Cut`
- `Drop`
- `SourceChange`
- backend 重建

同一帧的 retry 不能推进历史。若当前 ABI 的
`history_supported = 0`，第一版应按“当前帧 NR”处理，不声称已有
完整运动补偿时域。

### 5.4 颜色和曝光

第一版：

```text
color = SDR BGRA
exposure = nullptr
pre_exposure = 1
exposure_scale = 1
transfer_strength = 1
color_strength = 1
model_scale = 1
```

后续如果需要曝光，可接播放器的 SDR 曝光估算，但必须进入 cache key。

## 6. 需要修改的文件和职责

### 6.1 新增

```text
src/AmdLmxxfRuntimeHost.h
src/AmdLmxxfRuntimeHost.cpp
src/AmdLmxxfEvaluatorAdapter.h
src/AmdLmxxfEvaluatorAdapter.cpp
src/AmdNeuralEvidence.h
src/AmdNeuralEvidence.cpp
vendor/lmxxf/LmxxfNrApi.h
packaging/amd-runtime-lock.json
```

### 6.2 修改

| 文件 | 修改 |
| --- | --- |
| `CMakeLists.txt` | 新 AMD 源文件、运行目录复制、AMD-only 构建开关 |
| `RuntimePolicy.h/.cpp` | AMD Vendor ID、RDNA4 分类、runtime layout |
| `GpuPreference.h/.cpp` | AMD 设备选择与 LUID 记录 |
| `NeuralRenderTypes.h` | backend enum、后端中立证据、AMD failure |
| `NeuralWorkerProtocol.h` | 协议升到 v7，WireResult 加 backend/evidence |
| `OfflineNeuralRenderer.h/.cpp` | `Retained` 双后端、证据分支、发布门禁 |
| `NeuralPreflightProbe.cpp` | AMD preflight 分支 |
| `NeuralWorkerMain.cpp` | AMD 运行目录和初始化分支 |
| `NeuralCache.h/.cpp` | schema 6、backend 字段、AMD manifest 验证 |
| `NeuralReceipt.h/.cpp` | receipt 加 backend、runtime/weights/timing |
| `main.cpp` | preflight key、runtime directory、cache identity |
| `ResidentHelperPolicy.h` | AMD idle policy；第一版可禁用 resident |

## 7. 后端中立的证据链

### 7.1 结果字段

`NeuralRenderResult` 建议新增：

```cpp
enum class NeuralBackendKind {
    NvidiaRenoDx,
    AmdLmxxf,
};

struct NeuralBackendEvidence {
    NeuralBackendKind backend;
    bool runtimeReady;
    bool jobSubmitted;
    bool outputProduced;
    bool outputNonEmpty;
    bool timingValid;
    uint64_t acceptedFrames;
    uint64_t runtimeEvaluationCount;
    std::string runtimeVersion;
};
```

现有 feature 18 字段保留给 NVIDIA，不再作为所有后端的硬性成功条件。

### 7.2 AMD 发布条件

AMD 缓存条目至少要满足：

```text
runtimeReady == true
acceptedFrames >= frameCount
runtimeEvaluationCount >= frameCount
outputProduced == true
outputNonEmpty == true
无 device-removed / GPU-stall / timeout
receipt.json 可验证
```

`GetTimings().valid == false` 不单独判失败；它是诊断证据，不是视觉正确性证据。
但连续缺失 timing 和输出检查失败时，渲染必须失败。

## 8. 缓存身份设计

AMD 渲染键建议：

```text
backend=amd-lmxxf
source=<source digest>
width/height
application=<cache revision>
gpu=amd-rdna4-gfx1201
driver=<Adrenalin version>
hip=<hip runtime digest/version>
runtime=<Lmxxf runtime digest>
models=<weights + noise digest>
module-set=<SHA256SUMS digest>
pipeline=amd-lmxxf|1pass|fast|style1|net-auto|fit-large
settings=<AmdNeuralRuntime.ini digest>
range
```

不得复用 NVIDIA render key。

建议：

- `NeuralCache` schema 升到 6。
- `backend` 写入 identity、manifest、receipt 和 environment。
- 缓存目录仍可共用根目录，但 key 完全隔离。

## 9. 预检和 runtime lock

### 9.1 AMD preflight

建议增加：

```text
NeuralWorker.exe --amd-neural-preflight
```

检查：

1. AMD RDNA4 GPU。
2. `amdhip64_7.dll` 可加载。
3. `LmxxfNrRuntime.dll` 只导出并返回可用的 `LmxxfNrGetApi`。
4. API ABI 版本匹配。
5. `QueryCapabilities()` 的 ABI、尺寸和架构标签。
   注意当前实现会在 `Create` 之前返回 `hip_ready=0`，因此它不能作为
   预检前置失败条件；HIP 是否真的可用要在 `Create` / `PrepareFrame`
   后结合 `GetStatus` 和一次真实帧提交判断。
6. assets 目录和 SHA256SUMS。
7. 至少 24 个 HSACO 模块。
8. 权重目录可读。
9. 合成一帧执行一次 `PrepareFrame -> RecordInputs -> EnqueueHip -> RecordOutputs`。
10. 输出尺寸正确且不是全零。

回执字段：

```text
backend=amd-lmxxf
gpu
adrenalin
hip
runtime version/hash
weights hash
hsaco count
module set hash
flags snapshot
network tier
probe frame result
```

### 9.2 AMD runtime lock

`packaging/amd-runtime-lock.json` 至少覆盖：

- `LmxxfNrRuntime.dll`
- `native-game-tiled-assets/SHA256SUMS`
- shader 文件
- `HIP/gfx1200/*.hsaco`
- `HIP/gfx1201/*.hsaco`
- `AmdNeuralRuntime.ini`

权重单独记录：

- 仓库可再分发的文件用 lock 校验；
- 用户提供的 NVIDIA 权重只校验本机 hash 或清单，不进入发行包。

## 10. 构建和运行限制

### 第一版建议

```text
GPU：RX 9070 XT / gfx1201 优先
次选：RX 9060 / gfx1200
系统：Windows 11 x64
颜色：SDR
输入：BGRA8 或 runtime 接受格式
输出：BGRA8
渲染：1 pass
resident helper：先禁用，每个任务独立 worker
```

### 暂不实现

- RDNA2/RDNA3。
- HDR 模型输入。
- 外部 MV/depth。
- 多 pass 热切换。
- Lmxxf 的 Fast History 实验。
- AMD worker 常驻复用。

## 11. 分阶段实施

### 阶段 A：Runtime 探针

- 在 Windows 上动态加载 `LmxxfNrRuntime.dll`。
- 证明 API ABI 和 `QueryCapabilities`。
- 用合成帧跑通一帧。
- 输出 PNG 和 timing。

退出条件：1080p、1 pass 成功，输出不是黑帧或原帧。

### 阶段 B：独立 evaluator

- 新增 `AmdLmxxfEvaluatorAdapter`。
- 实现持久输入、命令列表顺序、readback。
- 用 `OfflineNeuralRenderer` 的注入路径跑短序列。
- 验证 seek/reset/retry。

退出条件：10 秒 1080p30 短视频得到完整神经视频。

### 阶段 C：生产 worker

- `Retained` 增加 AMD 分支。
- 增加 AMD evidence。
- 预检、runtime lock、receipt、cache key。
- 协议升到 v7。

退出条件：播放器可通过 `D` 完成 1080p 短片的渲染、缓存和对比播放。

### 阶段 D：直播和导出

- 接入现有分段渲染和 NV12/P010 捕获策略。
- 评估 `Lmxxf` 是否能在目标 GPU 上稳定跟上常见视频。
- 做 AMD 专用 pace prior。

退出条件：30 分钟连续播放无 worker 死锁、无设备移除、无缓存错误。

## 12. 验收标准

### 功能

- 播放器在 AMD GPU 上正常打开、播放和导出原片。
- AMD 神经渲染可以完成、缓存和复用。
- NVIDIA 构建和测试完全不受影响。
- 不安装 `dxgi.dll` 或 ReShade。

### 性能

- 记录 900、1080、1440、4K 输入的 GPU 时间和端到端帧率。
- 记录 1 pass 下的显存峰值。
- 明确哪些源分辨率能够直播、哪些只能离线渲染。

### 稳定性

- 连续 30 分钟渲染。
- seek、暂停、取消、设备移除和 worker 重启。
- 权重、模块或 runtime 换版本后必须缓存失效。

## 13. 最终判断

```text
方案可行：是
推荐后端：LmxxfNrRuntime
当前代码改动量：中等偏大
最小接入点：新增 AMD evaluator，不改主播放器 UI
不能绕开：协议、证据链、缓存 schema、运行目录和 preflight
第一版范围：RDNA4 + SDR + 1 pass + 离线缓存优先
```

如果按上述边界实施，可以保留播放器现有的缓存、对比、导出和时间轴逻辑，
同时把 AMD 模型执行隔离在独立 worker 中。这样风险是可控的，也不会把
ReShade 和第三方运行时硬编码塞进主播放器。
