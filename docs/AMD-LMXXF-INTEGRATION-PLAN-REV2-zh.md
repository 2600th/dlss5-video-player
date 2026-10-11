# DLSS 5 Video Player on AMD：LmxxfNrRuntime 接入方案修订版

_核对基线：播放器 `f42cb39` / 0.28.2，本地方案修订日期 2026-10-10。_

当前进度（2026-10-10）：

- 已固定 Lmxxf ABI 头：`third_party/lmxxf/LmxxfNrApi.h`。
  **2026-10-11 更新：已从上游 `a9b7a521` 的 ABI 1 换成
  `TheAutomatic/dlss-5-amd-project` `v1.10.5-alpha` 的 ABI 3（176 字节 / 21 函数 /
  `LmxxfNrCapabilities` 去掉 `hip_ready` / `LmxxfNrFrameInfo` 80→160 字节）。
  理由是随 `-gai` 包分发的 `LmxxfNrRuntime.dll` 硬拒 ABI 1，只接受
  `GetApi(3, 176)`。换头前它连协商都过不了。详见
  `third_party/lmxxf/README.md`。**
- 已加入纯 C++ ABI/能力校验：`src/AmdLmxxfApi.h`。
- 已加入 RDNA4/`gfx1200`/`gfx1201` 与运行目录判定：`src/AmdLmxxfPolicy.h`。
- 已加入运行时 session 调用状态机：`src/AmdLmxxfSession.h`。
- 已加入 Windows 动态加载器：`src/AmdLmxxfRuntime.h/.cpp`。
- 已加入 `AmdLmxxfApiTests`，当前在 Mac 上直接编译通过。
- Windows 11 迁移和安装步骤见
  [`WIN11-DEVELOPMENT-SETUP-zh.md`](WIN11-DEVELOPMENT-SETUP-zh.md)。
- D3D12 设备探针、资源上传和真实 runtime 预检尚未实现。

**2026-10-11 进度（阶段 A、B 均已跑通真机）：**

- **阶段 A 完成。** `tests/AmdLmxxfRuntimeProbe.cpp` 用真
  `LmxxfNrRuntime.dll` 走完 14 步链路，产出非黑、非原帧的 1080p 神经帧。
  `GetStatus` 报 `arch=gfx1201 match=luid hip=1 net=1920x1080 passes=1`。
- **阶段 B 完成。** `src/AmdLmxxfEvaluator.h/.cpp` 实现
  `INeuralFrameEvaluator`，由 `tests/AmdLmxxfEvaluatorSmoke.cpp` 经
  `OfflineNeuralRenderer` 的注入路径驱动。真机 300 帧（10s@1080p30）全绿：
  300/300 帧神经求值、编码器拿到 300 帧、墙钟 8112 ms（27.0 ms/帧，30 fps
  预算 33.3 ms/帧）、`recreates=0`（持久输入纹理未触发编解码链重建）、
  `releaseMarks=300 releaseMarkFailures=0`、输出实测来自
  `job.private_output`。seek/preroll/`ResetTemporal` 与编码器失败重试
  两条支路同样全绿；借用 device/queue 的路径也已覆盖（阶段 C 的形态）。
- 复现命令与实测数字见 `third_party/lmxxf/README.md` 末节。

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

> **阶段 B 的现状与这条例外的差距（已实测，未修）。** 阶段 B 实现的是
> **一次只飞一个 job 的全串行**：`list_` / `allocator_` 各一个，所以在录制
> consumer list 之前必须等 producer 执行完（`allocator_->Reset()` 不能在
> GPU 还在读旧命令时发生），读回前再等一次。也就是每帧两次 fence 等待，
> 而 5.2 要的是 2~3 帧在飞。
>
> 实测代价：300 帧 8112 ms 里，去掉首帧的 kernel 预热（1779 ms）后约
> **21 ms/帧**，仍在 30 fps 的 33.3 ms 预算内，但余量不大，且没有并发
> 掩盖延迟抖动的空间。
>
> 要做到 2~3 帧在飞需要一个资源环（多个 allocator / command list /
> readback buffer + 一个帧号-资源映射），这是阶段 C/D 的改动。
> 现在就不做，是因为它改变的是并发结构，需要重新验证 `Session` 的
> 单 job 假设、`private_output` 的状态轮转和读回时序——不该夹在
> 阶段 B 的收尾里顺手改。

### 5.3 时序重置

调用 `ResetHistory` 的时机：

- `FirstFrame`
- `Seek`
- `Cut`
- `Drop`
- `SourceChange`
- backend 重建

同一帧的 retry 不能推进历史。

> **实测修正（阶段 B）：光调用 `ResetHistory` 不会让时域模型上线。**
>
> 本节原先写的是"若 `history_supported = 0`，第一版按当前帧 NR 处理"。
> 真机读数推翻了这条判断的依据：`QueryCapabilities` 报
> `history_supported = 1`，但 300 帧全程 `GetStatus` 报 `history=off`，
> 即**输出是逐帧空域 NR，没有任何时域累积**。
>
> 读 runtime 源码（`TheAutomatic/dlss-5-amd-project` `v1.10.5-alpha`
> 的 `PrepareHistory`）后确认，时域要真正上线必须同时满足：
>
> 1. `LmxxfNrFrameInfo::temporal_flags` 带 `LMXXF_NR_TEMPORAL_MODEL_HISTORY`
>    **和** `LMXXF_NR_TEMPORAL_INPUTS_VALID`（后者是"运动/深度契约有效"的声明）。
> 2. `LmxxfNrCreateInfo::flags` 带 `LMXXF_NR_CREATE_FLAG_RECORDING_LEASES`。
>    非 lease 模式下 runtime 把 `historyStatus` 直接写成
>    `unsupported-lifecycle`，且 `requestHistory` 恒为 false——**这一点无法绕过**。
> 3. 宿主每帧提供**真实的运动向量和深度纹理**（`info.motion` / `info.depth`），
>    要求同一 device、格式和尺寸可识别、不互指也不指向 `color`、`motion_width/height`
>    不超界。
> 4. `motion_scale_x/y` 与 `jitter_x/y` 为有限值。
> 5. bridge 的 post-auxiliary 布局存在，且 `WaveOwnedCompatible(opt)` 为真
>    （需要 `post_merge_fold && post_head_fused`）。
>
> 所以上面那次 300 帧的 `history = off` 是**预期结果而非缺陷**：合成源没有引导
> 纹理，也从没声明这两个 flag。`history_supported = 1` 只说明 runtime 支持，
> 不代表宿主满足了契约。
>
> **实测结果（第一次）：通路已打通，收益由模型决定。**
>
> `AmdLmxxfEvaluator` 实现了 lease 生命周期（`src/AmdLmxxfSession.h` 的
> `BeginRecording` / `EndRecording` / `InvalidateRecording` / `CollectRecording`），
> 并按上面的门禁每帧提供恒定的运动与深度引导。`AmdLmxxfEvaluatorSmoke` 的
> run 5 在"静止内容 + 逐帧独立噪声"的序列上实测（24 帧）：
>
> ```text
> lease frames through this evaluator: 24
> history: active                      ← PrepareHistory 的每一条门禁都通过了
> 残差高频能量   空域 6.066  →  时域 5.944   （末 8 帧均值）
> 与空域输出的平均绝对差: 1.548         ← 历史确实参与了运算
> ratio temporal/spatial: 0.980
> ```
>
> **`history=active` 说明通路正确，但降噪收益只有 2%，而且原因不在宿主。**
> 读 kernel（`native_fast_history.h` 的 `Finish`）确认，融合权重是
>
> ```hlsl
> float weight = saturate(historyStrength) * .73974609375 / (1 + exp(-logit));
> ```
>
> 其中 **`logit` 是网络自己输出的一路通道**（`Network[logitOffset + i*2]`）。
> 也就是说"信任多少历史"是**模型学出来的策略，不是宿主可调的旋钮**。
> 按一阶 IIR（`v = (1-w)x + w*v_prev`，稳态噪声方差 `(1-w)/(1+w)`）反推，
> 本场景下 logit ≈ −3.6，对应权重 **0.020**：合成图案不在训练分布内，
> 模型给出的策略是"基本不信任历史"。实测的逐帧能量序列也印证了这一点——
> 它从第 1 帧起就是平的，没有任何累积趋势，因为稳态在第一帧就到了。
>
> 由此得到两条对阶段 C/D 有约束力的结论：
>
> 1. **权重上限是 0.7397**（`logit → +∞` 时），对应稳态噪声降到 38.6%。
>    这是这套时域模型的**天花板**；真实内容上的实际值只能实测。
> 2. **时域历史有一个硬性 disocclusion 守卫**，宿主无法放宽：
>    `HistoryCompatible` 在 `any(|上一帧原始像素 − 当前帧原始像素| > 0.08)`
>    （约 20/255）时判定历史不可用并放弃融合。意思是压缩块效应、闪烁或快速
>    运动一旦让同位置原始像素差超过 20/255，那一块就不累积。
>    run 5 最初用 ±16 的噪声（帧间差可达 31/255），**大部分像素被这个守卫拦掉，
>    降噪收益为 0**；把噪声降到 ±8（帧间差上限 15/255）之后才测出上面 0.980
>    的真实值。"先归零、再出现"这个过程本身就是守卫在起作用的证据。
>
> **产品含义：只做空域 NR 和做实时域 NR 之间的差别，不由我们决定，由模型对
> 具体内容的 logit 决定。** 所以在真实素材上测量出收益之前，不应把"时域累积"
> 计入预期质量收益。阶段 C/D 的正确顺序是：lease 通路先接上（已完成），
> 在真实视频上重跑同一度量拿到 logit/收益，**再**决定是否值得为它承担
> 运动估计的成本（见下面的成本说明）。
>
> **实测结果（第二次）：引导可以从文件逐帧喂进来，并且在运动内容上确实被读到。**
>
> run 5 的引导是两个常量，因此它只能回答"时域模型会不会累积"，回答不了
> "runtime 到底有没有采样那张运动纹理"——静止场景下两者的正确答案都是 0。
> 所以 `AmdLmxxfEvaluator` 增加了 `motionGuideDirectory` / `depthGuideDirectory`：
> 按 `src/GuideFiles.h` 的布局逐帧读 PFM（每帧一个文件，文件名是源自己的
> 0-based 解码序号，也就是 `FrameIdentity::frameNumber`，正是
> `tools/benchmark --guide-dump` 写出来的那种），`ResampleArea` 重采样到帧尺寸后
> 经同一张常驻纹理上传。纹理指针和尺寸仍然不动，只有内容每帧变——真实宿主也必须如此。
> 缺文件会让该帧失败，不会退回常量：悄悄混两种来源的运行，两种都不是测量结果。
>
> `AmdLmxxfEvaluatorSmoke` 的 run 6 在**平移内容**（每帧左移 8 px，三个通道三条
> 不同周期的正弦，含 ±8 噪声）上跑三条臂 × 两个噪声种子，共 6 次渲染：
> A 无 lease 空域参考、B lease + 真实位移场、C lease + 零位移场，三者的源/几何/
> 噪声完全相同。
>
> ```text
> guidesFromFiles=20 leaseFrames=20 history=active      ← 三条臂的引导都来自文件
> 同一帧、同一噪声种子下 B 与 C 的平均绝对差: 0.761          ← 一个浮点数的差别
> 同一帧下 B 与 A 的平均绝对差: 0.870
> ```
>
> **B 与 C 只差运动场里的那一个浮点数**（源、几何、噪声抽样完全一致），两者输出
> 却差了 0.761 级——这就是"文件里的位移向量真的进了网络"的判据，而常量引导
> 给不出这个判据。
>
> **降噪收益的测量在本次被重写，因为第一版量错了东西。** 第一版拿"合成帧减去
> 无噪声原图"的平均绝对差当指标，三条臂分别是 8.125 / 7.973 / 7.699——
> 而噪声本身的平均幅度只有 4.0，也就是说**一个什么都不做的臂反而会拿最高分**，
> 而且"被喂错运动"的 C 反而最好。那个数在量网络对正弦内容的重塑，不在量残余噪声。
> 正确的量法是同一臂跑两个噪声种子相减：内容与网络对该内容的偏置精确抵消，
> 剩下的就是没被去掉的噪声。改完之后：
>
> ```text
> 源本身（不做任何降噪）        : 5.312
> A  空域，无引导             : 5.325
> B  lease + 真实位移场        : 5.308
> C  lease + 零位移场          : 5.305
> B/A 0.997   C/A 0.996   B/C 1.001
> ```
>
> **结论：在这段合成内容上，时域通路几乎没有降噪收益，而且与运动场对不对无关**
> （0.997 / 0.996 / 1.001 全在噪声内）。这与 run 5 的发现完全自洽：
> 融合权重是网络学出来的 logit（本场景反推 0.020），不是宿主旋钮；
> 同时 0.08 的 disocclusion 守卫也在另一侧封顶——它只放行原始像素帧间变化
> 小于 20/255 的像素，所以一个视频源**最多**能被平均掉的噪声幅度是被它钉住的。
> 这条对 5.3 上面两条结论是补充而非修正。
>
> **产品含义不变，但证据更强了：现在有了一把可信的尺子，量出的是"没有收益"，
> 而不是"量不出来"。** 下一步是在真实素材上跑同一度量：真实视频的运动场由
> 估计得到（播放器侧现成的只有 `TemporalGuideGenerator` 的 160×90 分析网格，
> RGBA32F），把它 dump 成 PFM 就能走这条已经完全打通的路径。
>
> 运动估计的成本没有变：合成源可以用解析已知的位移，真实视频则需要
> ffmpeg 的运动估计或播放器侧的引导输出。播放器侧现成的只有
> `TemporalGuideGenerator` 的分析网格（160×90@1080p30，RGBA32F），因为
> 硬件光流是 NVIDIA 的 NVOFA（`nvofapi64.dll`），在 AMD 卡上
> `Initialize()` 必然失败，只剩 CPU 块匹配。
>
> 代码现状：`MakeFrameInfo` 默认 `temporal_flags = 0`；需要时域的调用方用
> `ApplyTemporalGuides` 填引导，视频场景应带 `kTemporalVideoFlags`
> （含 `MV_JITTERED`，因为解码帧没有引擎亚像素抖动）。
> 引导要么是 `motionPixelsPerFrameX/Y` + `depthValue` 两个常量，要么是上面那两个
> 目录，两者可组合（只给运动目录则深度保持常量）。
> `AmdLmxxfEvaluatorSmoke` 的 `historyResets >= 1` 断言只证明
> `ResetHistory()` 没报错；**证明历史真的上线的判据是 `HistoryStatus()`
> 读到 `active`**，证明文件引导真的被逐帧读了则看 `GuideFrames()`。
>
> **实测结果（第三次）：度量搬到真实解码视频上，权重是合成内容的四倍，但绝对值仍然很小。**
>
> 上面那句"下一步是在真实素材上跑同一度量"已经做了。`AmdLmxxfEvaluatorSmoke`
> 的 run 7 把**真实解码帧**喂进同一套度量：素材是仓库里就有的
> `tools/benchmark/fixtures/demo-capture-20260912.mp4`（1920×1080 / 30 fps /
> 678 帧，内容是播放器窗口的屏幕录制），解码走
> `tools/fetch_ffmpeg_helpers.ps1` 落的 `external/ffmpeg/bin/ffmpeg.exe` 子进程——
> 这**正是播放器自己的解码方式**（`VideoDecoder` 用 `CreateProcessW` 起 ffmpeg），
> 所以内容是真实纹理、真实压缩噪声、真实运动，而不是本测试自己发明的图案。
> 帧在 run 7 开头一次性解出来存在内存里，此后所有臂渲染的是同一批字节，
> 所以臂与臂之间的差别只可能来自引导。
>
> 同一个 clip 跑两个 subject，引导都是**常量**（零运动 + 单深度面）：
> `still` 每一帧都用 clip 的第一帧，内容是真正静止的、**零运动是真值**；
> `walking` 用 clip 自己的连续帧，内容在动、**零场是错的**。每个 subject 各跑
> 空域与 lease 两条路径、各两个噪声种子，共 8 次渲染。
> （"输入噪声"是同一估计量作用在输入自己身上的读数，即"完全不降噪"的分数；
> 两次运行逐位复现。）
>
> ```text
>                      输入噪声   空域              lease(零场)      比     隐含权重
> still  (真值引导)     4.561    4.871 (1.068×)   4.490 (0.984×)  0.922   0.081
> walking(错引导)       4.981    5.473 (1.099×)   5.334 (1.071×)  0.975   0.026
> run 5  (合成正弦静止)      ——             ——                0.980   0.020
> ```
>
> 四条结论：
>
> 1. **真实纹理上的融合权重是 0.081，合成正弦上是 0.020，差四倍。** 这回答了
>    run 5 留下的那个问号——权重既属于模型也属于内容，这个模型对合成光栅
>    确实更不信任。所以 run 5 的 0.020 是**低估**，不能当作模型的普遍权重。
> 2. **但即使是最有利的情况，绝对值也只有 8%。** `still` 是引导**全对**、
>    内容真正静止、守卫全部放行的最好情况，权重仍然只有 0.081。运动内容只会
>    更低：`walking` 的 0.026 就是守卫把运动区域拦掉、只剩静止区（播放器边框、
>    标题栏、控制栏）在融合的结果，两个 subject 的差正是守卫在起作用。
> 3. **空间路径自己在放大噪声。** 空域读数比输入高 6.8%（still）/ 9.9%（walking），
>    所以"降噪"的基线不是输入，而是网络自己处理过的输入；时域路径把它拉回到
>    0.984× / 1.071×。这条与权重无关，但它对解读任何"降噪收益"都成立：
>    拿输入当基线会把网络的放大算进时域的功劳里。
> 4. **与 run 6 自洽。** run 6 在合成运动内容上量到真场与零场没有差别
>    （0.997 / 0.996），run 7 在真实运动内容上用错场量到 0.026——两者都指向
>    同一件事：这段内容上时域通路能贡献的量被模型自己的 logit 钉在很小的数上。
>
> **对阶段 C/D 的判断：为运动估计付成本的收益上界是噪声标准差下降 7.8%**
> （`sqrt((1-0.081)/(1+0.081))` = 0.922），而且这个上界落在最有利的静止内容上；
> 真实视频的运动场本身还有估计误差，只会更差。**这个上界不足以支持先写一个
> CPU 块匹配器。** 现在的状态是"用真实素材量过了，量出的是收益很小"，
> 比 run 6 的"合成内容上没有收益"强，但不足以改变结论。
>
> 仍未验证的两项与 run 6 相同：位移向量的**符号**约定仍是照 `GuideFiles.h` /
> `TemporalGuides` 两个头声明的，本次没有独立验证 runtime 重投影的符号；
> 以及**真实运动场**（而不是常量场）在真实内容上的读数还没有取到。
>
> 代码现状补充：run 7 用到的两个路径来自编译期定义
> `DLSS5_FFMPEG_PATH` / `DLSS5_REAL_CLIP_PATH`（见 `tests/HardwareTests.cmake`），
> 两者任一缺失时 run 7 自己跳过而不是失败——所以一个没有 staged ffmpeg 的
> checkout 仍然跑得完这个 target 的其余全部内容，`external/*` 也本来就在
> `.gitignore` 里，下载的工具不会进仓库。

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

已落地（阶段 A、B，实际文件名为准，与本节原先的草案名有一处差异）：

```text
third_party/lmxxf/LmxxfNrApi.h          ABI 3 头（固定自 v1.10.5-alpha）
third_party/lmxxf/NrPerformance.h       仅为 LmxxfNrTimingApi 类型
src/AmdLmxxfApi.h                       纯 C++ ABI/能力校验、MakeFrameInfo
src/AmdLmxxfPolicy.h                    AMD Vendor ID、gfx1200/gfx1201、runtime layout
src/AmdLmxxfSession.h                   调用状态机（含 phase 与 LastStatus）
src/AmdLmxxfRuntime.h/.cpp              动态加载、Create、PrepareSession、GetStatus/Timings
src/AmdLmxxfEvaluator.h/.cpp            INeuralFrameEvaluator 的 AMD 实现
tests/AmdLmxxfApiTests.cpp              ABI/能力纯逻辑测试
tests/AmdLmxxfRuntimeProbe.cpp          阶段 A 探针（14 步链路）
tests/AmdLmxxfEvaluatorSmoke.cpp        阶段 B 注入路径集成测试（6 个 run）
```

未新增文件但被 evaluator 复用的一个既有头：`src/GuideFiles.h`（PFM 读写与
`ResampleArea`，原本只服务 `tools/benchmark` 的 `mv=file:` 路径）。
`AmdLmxxfEvaluator` 的 `motionGuideDirectory` / `depthGuideDirectory` 直接读它定义的
布局，所以 benchmark 用 `--guide-dump` 导出的引导可以原样喂给 evaluator —— 这正是
阶段 C/D 需要的"真实运动估计结果进出同一条路"。

**命名差异：** 草案里的 `AmdLmxxfRuntimeHost` 与 `AmdLmxxfEvaluatorAdapter`
最终叫 `AmdLmxxfRuntime` 和 `AmdLmxxfEvaluator`；`AmdLmxxfRuntimePolicy`
的选择逻辑落在 `AmdLmxxfPolicy.h`（不单独成对）。`OfflineNeuralRenderer`
自带的 `InjectedEvaluatorAdapter` 已经承担了适配职责，所以没有再造一层
adapter——`INeuralFrameEvaluator` 本身就是接缝。

仍待新增（阶段 C 及之后）：

```text
src/AmdNeuralEvidence.h/.cpp            后端中立证据结构
packaging/amd-runtime-lock.json         runtime SHA-256 与 ABI 锁定清单
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

> **实现（2026-10-11）：没有新增 `NeuralBackendEvidence`，wire 布局也没动。**
> 落地的是比 7.1 更小的一组，因为根本问题不是"缺字段"，而是 `requireNeural`
> 被同时当成了两件事：job 对自己帧的**声明**（`NeuralRenderResult::neural`，
> 会随 wire 回传并被父进程逐字段校验），和"要用 NVIDIA 那套 receipt 判据"的
> **开关**。把后者绑在前者上，就是第一版 AMD 接线失败的原因：helper 把每个 AMD
> job 都发布成"Super Resolution-only"，父进程按 `AcceptResult` 拒收。
>
> 三处改动：
>
> - `NeuralRenderJob::NvidiaEvidenceRequired()` = `requireNeural && backend != Amd`。
>   它驱动四处 NVIDIA 专有判据：receipt 的 `evidence.Valid()`、
>   `highestObservedEvaluation` 基线推进、NGX 校准的 `NeuralGpuMsFloor`、
>   `holdToLogReceipt_`，以及 `ArmBeforeCapture` 整段（含 re-hook）。`requireNeural`
>   本身不再被任何一处清零。
> - `DecodeResult(payload, backend)`：NVIDIA 仍要整套 feature 18；AMD 要
>   `verifiedNeuralFrames >= frameCount && nativeEvaluations >= frameCount`，
>   且 feature-18 字段必须为空——报了 arm 就是跑错了后端，宁可拒收。
>   两组**互不通过**：AMD 的结果按 NVIDIA 规则解会被拒，反之亦然，
>   这正是后端必须作为参数传进去的理由。
> - §7.2 的条件映射到已有字段：`outputProduced`/`outputNonEmpty` 由原有判定覆盖，
>   `acceptedFrames >= frameCount` 就是 `verifiedNeuralFrames`，
>   `runtimeEvaluationCount` 就是 `nativeEvaluations`（AMD 上
>   `EvaluationCount` 和 `NeuralEvaluations` 是同一个计数器，见适配器注释）。
>   `receipt.json` 可验证性要等 AMD 缓存身份（§8）落地才有意义。
>
> 副作用是 `IsReusableNeuralCacheManifest` 目前对 AMD 条目一律返回 false
> （它仍要求 `feature18Created` 等三项），所以 AMD 的渲染**能出片但读不回缓存**。
> 这是 fail-closed，不是错误输出，但属于 §8 的欠账。

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
   ABI 3 已删除 `hip_ready` 字段（ABI 1 时代它在 `Create` 之前恒为 0），
   因此 HIP 是否真的可用只能由 `GetStatus` 在 `Create` / `PrepareFrame`
   之后报出，再结合一次真实帧提交判断。`gfx1201_target` 也已在 ABI 3 标记
   deprecated，活动架构同样看 `GetStatus`。
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

> **已完成（2026-10-11）。** `tests/AmdLmxxfRuntimeProbe.cpp`，14 步链路全过，
> exit 0。真机实测：`arch=gfx1201 match=luid pdl=1/1(on) hip=1 net=1920x1080
> passes=1 network_passes=1 predict=1 net_gpu_ms≈8.9`，`releaseMarks` 计数与
> 提交帧数一致且 `releaseMarkFailures=0`。

### 阶段 B：独立 evaluator

- 新增 `AmdLmxxfEvaluatorAdapter`。
- 实现持久输入、命令列表顺序、readback。
- 用 `OfflineNeuralRenderer` 的注入路径跑短序列。
- 验证 seek/reset/retry。

退出条件：10 秒 1080p30 短视频得到完整神经视频。

> **已完成（2026-10-11）。** 实际实现叫 `AmdLmxxfEvaluator`
> （`src/AmdLmxxfEvaluator.h/.cpp`），没有单独造 adapter——`INeuralFrameEvaluator`
> 本身就是接缝，`OfflineNeuralRenderer` 已有的 `InjectedEvaluatorAdapter` 已够用。
>
> 真机 300 帧（10s@1080p30）结果：`frames=300 nativeEvaluations=300`，
> 墙钟 8112 ms / 27.0 ms 每帧，`recreates=0`，`bridgeCreates=1`，
> `releaseMarks=300 releaseMarkFailures=0`，`net_gpu_ms=8.858`。
> 三个 run 都验证过：整段序列、有界 range（seek + preroll + `ResetTemporal`，
> `source opens=2 seeks=2`）、编码器失败重试（NVENC → 软件，12 帧两遍）。
> 第四个 run 借用外部 device/queue，覆盖阶段 C 的形态。
>
> 阶段 B 暴露并已修正的 ABI 3 语义陷阱，三条都是实测确认：
>
> 1. **`job` 参数在 `PrepareFrame` 之后是 handle，不是 `&job`。**
>    runtime 走 `Session::FindJob(handle)`，非录制模式只认 `nullptr` 或 session
>    内部 job 地址。传 `&job` 会得到 "need job and command list"，而链路上
>    每个指针都非空。
> 2. **成品帧在 `job.private_output`（B8G8R8X8_UNORM，UAV）。**
>    宿主彩色纹理是 codec 的**输入**，不是输出。读错纹理不会报错，只会安静地
>    把输入原样交出去。
> 3. **`shaders/` 是独立目录**，`shaders/native_codec_encode.hlsl` 与
>    tiled assets 下的同名文件**内容不同**，不能互相替代。
>
> 另外一条工程结论：**每帧换颜色资源指针/尺寸/格式/曝光绑定会重建或重绑
> 编解码链并可能等 GPU**，所以 `color_` 在 `Initialize` 里创建一次、指针永不移动，
> 每帧只 `CopyTextureRegion` 上传内容。`recreates=0` 就是这个策略的直接读数。
>
> **命令列表状态机必须全程走 `Session`。** 首次真机失败就是因为
> `PrepareFrame` 走了 `Session`、其后四个调用直接打了裸 API 表项，导致
> `Session::phase_` 卡在 `Prepared`，`Retire` 判 `CanRetire` 为否而拒绝，
> 报出的却是"每个调用都 OK"。`Session` 现在还会暴露 `LastStatus()`，
> 好让调用方仍能区分 `DEVICE_LOST` 和普通失败。
>
> 退出条件达成：`AmdLmxxfEvaluatorSmoke` exit 0，`RESULT: ok`。
>
> 后续在阶段 B 内追加了两个 run，都属于"把时域这条路的问号拆开"：
>
> - **run 5**（恒定解析引导，静止内容）：证明 lease 生命周期通了、
>   `history=active`、时域确实参与运算。见 5.3。
> - **run 6**（逐帧文件引导，平移内容）：证明引导可以按源帧号逐帧读进常驻纹理，
>   并且**运动向量的值真的被网络用到了**（同一帧、同一噪声抽样下，真实位移场与
>   零位移场的输出差 0.761 级）。同时把降噪收益的度量改成"同臂双噪声种子相减"，
>   量出的结论是这段合成内容上没有可测收益。见 5.3。
>
> run 6 也是阶段 C/D 的前置件：真实视频要的正是"把算出来的 motion/depth 写进
> 文件、再由 evaluator 读进来"这条已经跑通的路径，而如何算出运动场仍是一个
> 独立未决项（见 5.3 末段）。
>
> - **run 7**（真实解码视频，静止与运动两个 subject）：把同一套度量搬到
>   `tools/benchmark/fixtures/demo-capture-20260912.mp4` 解码出的真实帧上，
>   解码走 ffmpeg 子进程（就是播放器自己的解码方式）。量到真实纹理上的融合
>   权重是 0.081，是合成内容 0.020 的四倍——run 5 的 0.020 是低估；但**即使
>   引导全对、内容真正静止，也只有 8%**，运动内容上更低（0.026）。顺带发现
>   空域路径自己会把噪声放大 6.8%~9.9%，所以降噪的基线是网络处理过的输入
>   而不是输入。见 5.3。
>
> 最终验证读数（300 帧全量）：**97 条断言全过**、exit 0；
> `build-ci` 全量 0 warning / 0 error；`ctest --preset quick` 16/18，
> 两条失败（`RenderSettingsTests`、`PolicyTests`）与本次改动无关，为既存失败
> （失败点在 youtube helper 相关用例上，而本次只动了 `tests/`，没碰 `src/`，
> 而 `PolicyTests` 检查的是 `src/` 里的进程调用）。

### 阶段 C：生产 worker

- `Retained` 增加 AMD 分支。
- 增加 AMD evidence。
- 预检、runtime lock、receipt、cache key。
- 协议升到 v7。

退出条件：播放器可通过 `D` 完成 1080p 短片的渲染、缓存和对比播放。

> **已接线（2026-10-11），走的是生产路径而不是注入路径。** 实际落点：
>
> - `AmdLmxxfEvaluatorAdapter`（`OfflineNeuralRenderer.cpp`）按 `NeuralRenderJob`
>   的 duck-typed 面实现，与 `ProductionEvaluatorAdapter` 并存于 `Retained`，
>   换卡不重建、NVIDIA 路径不被触达。刻意不提供
>   `PrepareDirectEncode`/`SceneCuts`/`StageDetail`，靠已有的
>   `if constexpr (requires {...})` 逐条降级。
> - `NeuralRenderRequest::backend`（`nvidia-ngx` / `amd-lmxxf`）是本轮唯一新增的
>   叶子类型，因为它要同时进命令行、导出计划、worker 协议和将来的 cache key。
> - worker 协议沿用 v6 布局，只加了一个键 `--neural-backend`；缺省即 NVIDIA，
>   所以旧 helper 的字节序列不变。AMD 明确拒绝常驻会话（一行一个 job）。
> - runtime lock 改为后端中立：AMD 树用自带的 `SHA256SUMS` 校验，
>   `LmxxfNrRuntime.dll` 本身只有**存在性**检查（它的可信来源是 git，不是该函数）。
> - ReShade 契约整块按后端跳过：`ReShade.ini` 的修复、add-on 开关、
>   `UnlockedRuntimeModules`、常驻 helper 清退。
>
> **协议 v7 没有做，也不需要做。** 结论见 §7.1 的修订：证据链的后端差异不需要
> 新的 wire 字段，因为两套证据集没有一个字段重叠，而父进程永远知道自己启动了哪个
> helper——把后端作为解码参数传进去，比在 wire 上加一个字节更严格（见 §7.1）。
>
> 实测（RX 9070 XT / gfx1201，1080p30，`--range f0-f8`）：
> `DLSSVideoPlayer --render … --stages nr --neural-backend amd --out …` exit 0，
> 产出 1920x1080 H.264 mkv（217 324 字节，8 帧），`nativeEvaluations=8`，
> `submit` 241 ms/帧（含首帧预热），`local-vram` 1343 MiB / 15416 MiB。
> 同一个二进制不带 `--neural-backend` 仍走 NVIDIA 路径且行为不变。
>
> **已经补上的（2026-10-11 第二轮）：**
>
> 1. ~~**AMD 缓存身份（§8）没做。**~~ **已做。** `NeuralCacheManifest` 加了一个
>    可选 `backend` 字段（写在 `environment` 之后，**只在非默认时写**，所以 NVIDIA
>    条目与加这个字段之前逐字节相同，加之前写下的条目读回来即 NVIDIA）。
>    `IsReusableNeuralCacheManifest` 按后端分流：AMD 条目问的是**不需要 add-on 的
>    那一对**——runtime 自己的逐帧计数覆盖了每一帧，且每一帧都是 verified neural
>    output——同时要求三列 add-on 计数**取反**（`!feature18Created &&
>    !feature18ArmedBeforeCapture && observedFeature18Evaluations == 0`）而不是忽略：
>    一个自称 AMD 却记着 add-on 跑过的条目是自相矛盾的，服务它等于发布没人核对过的帧。
> 3. ~~**直播（`D` 键）没接。**~~ **已接。** 走的是与离线同一条 `RunJob`，见下。
>
> **仍然缺的：**
>
> 2. **时序模型在播放器路径上不可达。** runtime 只在 recording-lease 生命周期下
>    跑时序模型，而它融合的 guide 是调用方给的，这个后端没有 estimator——
>    这是 §5.3 明确留给后续阶段的"贵的那一半"。所以 AMD 导出目前是**纯空间**渲染，
>    日志里以 `temporal=off, …this job brought no guide files` 明写，
>    末尾再用 `history=not-asked(no lease frame ran)` 复述一次：
>    时序没跑和时序跑了但报 `off` 是两件事，不能都留空。
>    带 guide 文件的基准（`mv=file:`/`depth=file:`）会打开 lease 生命周期，
>    这是 `AmdLmxxfEvaluatorSmoke` run 6/7 测过的路径。
> 4. **阶段 D 的捕获策略没接**：`CapturePixelFormat` 固定 BGRA，
>    NV12/P010 的 GPU 转换路径在 AMD 上不存在，编码器因此选 H.264 软件路径
>    或 FFV1（`RunJob` 里的 `selected` 分支），NVENC 直连不可能。
> 5. **pace prior 没做**（§12 性能里的 AMD 专用档），直播目前用通用预算。

### 阶段 D：直播和导出

- 接入现有分段渲染和 NV12/P010 捕获策略。
- 评估 `Lmxxf` 是否能在目标 GPU 上稳定跟上常见视频。
- 做 AMD 专用 pace prior。

退出条件：30 分钟连续播放无 worker 死锁、无设备移除、无缓存错误。

> **直播已接线（2026-10-11 第二轮），离线导出的产物不变。**
>
> 广播路径与离线共用 `NeuralRenderRequest` 和 `OfflineNeuralRenderer::Run`，
> 所以这一轮没有第二套渲染代码，只有"这个 job 用哪个后端"这一条线：
>
> - **启动时选一次后端**（`RunNeuralAddonBootstrap`）。NVIDIA 布局分类为
>   `Absent` 时不再直接落到 DLAA 模式，而是先探 AMD：`ResolveRuntimeDirectory`
>   解出包 → `VerifyAmdRuntimeLock` 对着包自带的 `SHA256SUMS` 校验 →
>   检查同包内的 `NeuralWorker.exe`。三样都过就把 `neuralAddonConfigured`
>   置真并记下这个目录。D 键的可用性、状态行、`--capabilities` 都读这个标志，
>   所以**一次判定，处处一致**。安全模式仍然排除在外：它是"什么都没加的这个
>   播放器"，第二个后端正是"加的东西"。
> - **每个 job 的 runtime 与预检按后端分流**（`NeuralJobRun::PrepareAmdRuntime` /
>   `ClearAmdPreflight`）。AMD 侧：runtime 身份文件由**遍历包**得到
>   （`AmdRuntimeIdentityFiles`，跳过 runtime 自己写的 `shaders/shader-cache/`，
>   否则"缓存有没有热过"会改变渲染身份），逐 job 重校锁，租约仍是命名互斥量。
>   被跳过的是整块 NVIDIA 预检——驱动门限、`UnlockedRuntimeModules`、
>   五秒 feature-18 探针——它们问的都是 add-on 或 feature 18 的性质，
>   这个后端两样都没有。**留下的两条拒绝**是"不支持即拒绝"而不是近似：
>   `processing-scale != 100`（runtime 只收固定盒子，缩小会被静默忽略）与
>   任何放大。
> - **常驻会话对 AMD 直接判不可行**（`ResidentCanCarry` 对 Amd 返回 false），
>   于是 `RunJob` 落到单进程 `RunNeuralWorker`，与 `NeuralWorkerMain` 里
>   "AMD 没有常驻会话"的拒绝同源。判在这里而不是让它去撞拒绝，是因为
>   常驻 helper 是 NVIDIA 的启动行拉起来的（已经加载了 proxy、armed 了 add-on），
>   把 job 的后端通过命令通道告诉它，正好是那个进程明确拒绝的情况。
> - **包目录随后端一起过 wire**（`--amd-runtime-dir`）。父进程已经解出并校验过
>   一棵树，让 helper 自己再解一次，两者只是"碰巧一致"；传过去才是
>   "渲染加载的就是播放器说它要用的那棵"。缺这个键是旧版父进程的情况，
>   helper 退回自己解，和加这个键之前一样。
> - **发布时不再复读 `ReShade.ini`**。NVIDIA 路径要复读是因为 helper 会重写它；
>   AMD 的配置在请求里走，没有文件可读。原来的无条件复读会让**每一次** AMD 发布
>   失败。缓存清单同时记下 `backend`，`IsReusableNeuralCacheManifest` 按后端分流，
>   AMD 条目因此可以读回来。
>
> **实测（RX 9070 XT / gfx1201）：**
>
> - 播放器启动：`AMD neural runtime available; backend=amd-lmxxf
>   runtime=…\amd-neural-runtime verified=208 files against the pack's own
>   SHA256SUMS, plus LmxxfNrRuntime.dll by presence helper=…\neural-runtime\
>   NeuralWorker.exe GPU=AMD Radeon RX 9070 XT`。随后 D 之前的预渲染 job 走到
>   `AMD neural runtime identified: digest 1565 ms over 610 files, lock check 261 ms`，
>   再 `Neural cache miss`（AMD 第一次渲染，缓存还空）。
> - **范围渲染（直播每次执行的形状）已有硬件冒烟**：`NeuralRangeRenderSmoke`
>   加了 `amd [runtime-directory]` 分支，注册为 `NeuralRangeRenderAmdSmoke`
>   （`gpu` 标签，无包时 125 SKIP，所以 RTX 机器上跑这一条是跳过而不是变红）。
>   实测 `<ffmpeg> <NeuralWorker.exe> <out> amd amd-neural-runtime` exit 0：
>   `frames=72/72 verified=72 native_evaluations=72 feature18_armed=0
>   evidence_valid=0 preroll=60 elapsed=12.3s`——**72 帧全部经 runtime 求值**，
>   而两条 add-on 判据是**取反**通过的（这正是 §7.1 里 AMD 证据集的内容）。
>   640x360、24 fps 下 12.3 s / 72 帧 ≈ 170 ms/帧（含 bring-up），
>   这张卡上 1080p 的端到端数据见 §12。
>
> **实测没覆盖的部分（要说清楚）：** 上面走的是 `RunNeuralWorker`，也就是
> 播放器 live session 用的那一条；`NeuralJobRun` 里的
> `PrepareAmdRuntime`/`ClearAmdPreflight` 只有启动与预渲染阶段被实机走到
> （日志见上），**D 键之后的分段产出与缓存命中还没有实机跑过**——
> 需要 GUI 按键，本轮没有可用的自动化。所以 §12 的退出条件
> （30 分钟连续播放）**仍未验证**。

## 12. 验收标准

### 功能

- 播放器在 AMD GPU 上正常打开、播放和导出原片。
- AMD 神经渲染可以完成、缓存和复用。
- NVIDIA 构建和测试完全不受影响。
- 不安装 `dxgi.dll` 或 ReShade。

> **2026-10-11 的状态（第二轮结束后）：**
>
> | 条 | 状态 |
> |---|---|
> | 打开、播放、导出原片 | ✅ |
> | 神经渲染完成 | ✅ 离线 `--render` exit 0；范围渲染（直播的形状）硬件冒烟 exit 0 |
> | 神经渲染**复用** | ✅ 缓存身份按后端分流，AMD 条目可读回 |
> | 播放器内 `D` 键直播 | ✅ 已接线（启动选路 + 每 job 分流 + 单进程 helper + 发布） |
> | NVIDIA 不受影响 | ✅ 见下 |
> | 不装 `dxgi.dll` / ReShade | ✅ AMD 路径整块跳过 add-on 契约 |
> | 30 分钟连续直播 | ❌ **未验证**（需要 GUI 按键，本轮没有自动化） |
>
> "NVIDIA 不受影响"仍是逐项核对的结论，不是推断：`ctest --preset quick` 18 项里
> 2 项失败（`PolicyTests` 的 yt-dlp fixture、`RenderSettingsTests:857` 的
> symlink 拒删），**在未改动的 HEAD 上失败得一模一样**；失败点全在 YouTube
> resolver 与缓存符号链接，与神经后端无关。同一台机器上默认（NVIDIA）分支的
> `NeuralRangeRenderSmoke` 仍停在 `Unable to determine a readable ReShade.ini size`
> ——它本来的环境性失败。`NeuralWorkerTests` exit 0（含此前新增的 5 条 AMD
> 协议断言）。全量构建 0 error / 0 warning（`/W4 /WX`）。

### 性能

- 记录 900、1080、1440、4K 输入的 GPU 时间和端到端帧率。
- 记录 1 pass 下的显存峰值。
- 明确哪些源分辨率能够直播、哪些只能离线渲染。

> **1080p 已有实测（阶段 B，2026-10-11，RX 9070 XT）：**
>
> - 端到端 21 ms/帧（去掉首帧预热），27.0 ms/帧（含预热摊到 300 帧）。
>   30 fps 预算 33.3 ms，因此 1080p30 离线渲染有真实余量。
> - runtime 自报 `net_gpu_ms=8.858`（网络本身的 GPU 时间，只是端到端的一部分）。
> - 1 pass 显存：进程 local segment 用量实测 1343 MiB（首轮采样）到 2511 MiB
>   （四个 run 全部跑完后的采样），适配器 budget 15416 MiB。
>   注意 `QueryVideoMemoryInfo` 的 `CurrentUsage` 是**整个进程**的用量，
>   不是 runtime 单独占的，所以它适合做预算判断，不适合当成模型大小。
> - **900 / 1440 / 4K 尚未测**。注意 runtime 的 admitted box 是
>   1920x1080（`max_input_width/height`），且按**像素预算**准入：宽到 2560
>   而 `width*height` 不超过 `1920*1080` 的超宽输入也接受，但 1440p/4K
>   超预算，需要先用播放器的缩放路径落到预算内再送进去。

> **直播的形状（范围渲染，2026-10-11 第二轮，RX 9070 XT）：**
> `NeuralRangeRenderSmoke … amd amd-neural-runtime` 在 640x360、24 fps 上渲染
> `[1,4)` 秒、带 60 帧 preroll：72 帧 **12.3 s 全部完成**（含 helper 启动与
> bring-up），即 ≈170 ms/帧。这是含冷启动的数，不是稳态。
> **这个尺寸下的结论只能说明"范围+preroll 这条形状在 AMD 上走得通"**，
> 1080p 能否跟上 30 fps 要看上面那条 21 ms/帧的稳态数据，而它来自整片渲染
> （无 preroll、无分段）。

### 稳定性

- 连续 30 分钟渲染。**未做。**
- seek、暂停、取消、设备移除和 worker 重启。**未做。**
- 权重、模块或 runtime 换版本后必须缓存失效。**机制已有**：runtime 身份是
  遍历整包算出的摘要（`AmdRuntimeIdentityFiles` + `BuildRuntimeDigest`），
  换模块或权重即换身份；锁校验换成每 job 一次，漂移的树在启动和每个 job
  两处被拒。

**已知 flaky 测试（既有，与本轮改动无关）：** `NeuralPrerenderTests` 的
`quarantined_entries_are_kept_for_inspection_then_reaped_test` 会间歇在
`:1993`（期望 `SweepStaging()==1`，实得 `0`）和 `:1994`（隔离目录仍存在）
两处失败。机制已静态确认，不是猜测：`RemoveTreeBefore`（`NeuralCache.cpp:753`）
在 `now() >= deadline` 时直接 `return false` 且不删任何东西，而
`deadline = started + kSweepBudget`（100 ms），`started` 取在 `SweepStaging()`
顶部、**在枚举候选目录之前**（`NeuralCache.cpp:1454` vs 1484 的循环）。于是
「枚举 `staging/` + 逐条 `last_write_time` + 排序」只要耗掉 100 ms，循环就一次
都不删、`removed` 保持 0——正是上述两条断言的样子。本机 `fs::remove` /
`last_write_time` 实测会间歇耗时 100–1459 ms（临时 DIAG 读数），所以这个窗口
是够得着的。

产品行为本身是设计如此（预算含枚举，超时就把活留给下一次 sweep，最坏是不删），
**脆的是测试的假设**——"5 个文件的目录必须在一个预算内删掉"。判定它既有的依据
是 A/B：`git stash` 到未改动 HEAD 构建的基线二进制，同一窗口内 3 次跑出
**2 失败 1 通过**，而带本轮改动的二进制 3 次全过；上一轮会话里方向正好相反
（基线过、带改动失败）。两个二进制都各自出现在失败与通过的**两侧**，所以它是
环境时序噪声，不是本轮引入，也不是本轮修复。要收拾的话应改测试（把"预算内
必须删掉"换成"重试到删掉"），而不是把预算改成不含枚举——那会让 UI 线程上
多花的时间不再受约束。

## 13. 最终判断

```text
方案可行：是
推荐后端：LmxxfNrRuntime
当前代码改动量：中等偏大
最小接入点：新增 AMD evaluator，不改主播放器 UI
不能绕开：协议、证据链、缓存 schema、运行目录和 preflight
第一版范围：RDNA4 + SDR + 1 pass + 离线缓存优先
已实测：阶段 A、B 在 RX 9070 XT（gfx1201）上跑通，1080p30 × 300 帧全绿
        阶段 C 的接线 + 阶段 D 的离线导出已跑通：`--render --stages nr
        --neural-backend amd` 真出片（1080p H.264，exit 0，8 帧）
        （第二轮）缓存身份按后端分流，AMD 条目可读回；
        播放器启动即在 AMD 后端上完成选路（日志 verified=208 files）；
        范围渲染 + preroll（直播的形状）硬件冒烟 exit 0：72/72 帧、
        72 次求值、feature18_armed=0
剩余未知：AMD 时序模型（缺 guide estimator，当前为纯空间渲染且日志明写）、
          900/1440/4K 输入、30 分钟连续直播、设备移除恢复、
          AMD 专用 pace prior、
          D 键之后的分段产出与缓存命中（需 GUI 按键，本轮无自动化）
```

如果按上述边界实施，可以保留播放器现有的缓存、对比、导出和时间轴逻辑，
同时把 AMD 模型执行隔离在独立 worker 中。这样风险是可控的，也不会把
ReShade 和第三方运行时硬编码塞进主播放器。
