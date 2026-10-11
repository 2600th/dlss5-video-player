# LmxxfNrRuntime API 固定说明

来源：

```text
LmxxfNrApi.h   TheAutomatic/dlss-5-amd-project  tag v1.10.5-alpha
               OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h
NrPerformance.h 同上仓库  OptiScaler/dlssnr/NrPerformance.h（仅为 LmxxfNrTimingApi 类型）
```

本目录只固定集成所需的 C ABI 头文件。运行 DLL、HIP 模块、shader 和模型权重
不在本仓库内，需要由用户通过匹配的完整包或自行构建取得。

## 为什么是 ABI 3 而不是上游 a9b7a521

上游 `lmxxf/dlss5-on-amd-9070xt-porting`（本仓库其它部分固定的就是它的
`a9b7a521`）只到 **ABI 1 / 136 字节 / 16 函数**，全部 8 个分支和 tag 都是这个版本。
而随 `OptiScaler-NR-1.10.5-alpha-gai` 分发的 `LmxxfNrRuntime.dll` 硬拒 ABI 1：

```text
LmxxfNrGetApi(1, 136) -> 1  UNSUPPORTED_ABI
LmxxfNrGetApi(1, 176) -> 1  UNSUPPORTED_ABI
LmxxfNrGetApi(3, 136) -> 2  INVALID_ARGUMENT
LmxxfNrGetApi(3, 176) -> 0  OK
```

该 DLL 的真实实现（`0x1800A4BF0`）内联了两条断言：`abi_version == 3` 且
`struct_size == 0xB0`。ABI 3 相对 ABI 1 是**严格追加**：多了 4 个录制函数
（`BeginRecordingExecution` / `EndRecordingExecution` / `InvalidateRecording` /
`CollectRecording`）和 `GetTimings`，并把 `LmxxfNrCapabilities::hip_ready`
（改由 `GetStatus` 提供）去掉、把 `LmxxfNrFrameInfo` 从 80 字节扩到 160 字节。

`TheAutomatic/dlss-5-amd-project` 正是发布那个包的项目，其 `v1.10.4.1` /
`v1.10.5-alpha` tag 的头文件实测就是 ABI 3 / 176 字节 / 21 函数，与 DLL 逐项吻合。

## 使用约束

- 运行 DLL 必须由播放器的 AMD runtime lock 校验 SHA-256。
- API 表、帧结构和调用顺序由 `src/AmdLmxxfApi.h` 与
  `src/AmdLmxxfSession.h` 统一校验。
- 不要在业务代码中直接 `LoadLibraryW(L"LmxxfNrRuntime.dll")`；
  统一走 `src/AmdLmxxfRuntime.h`。
- 更新固定提交时必须重新核对 ABI、结构体大小、模块数量和运行锁。
- **换回 ABI 1 的 runtime（例如 AMDNR v0.3.5.x）需要同时把本目录的头换回
  `a9b7a521` 版本，否则 `LmxxfNrGetApi` 会在协商阶段被拒。**

## 播放器侧的接入（2026-10-11 起）

一个二进制带两条神经后端，job 自己说它要哪条（`--neural-backend nvidia|amd`）。
AMD 这一侧的形态，写在这里是因为它改变了"怎么放这个包"：

- **包的定位**：`src/AmdLmxxfRuntime.cpp` 的 `ResolveRuntimeDirectory()`，顺序是
  显式目录 → 环境变量 `DLSS5_AMD_RUNTIME_DIR` → 模块目录及其父/祖父下的
  `amd-neural-runtime`（helper 多一层 `neural-runtime/`，也认）。
  播放器启动时解一次并**把结果随 job 传给 helper**（wire 上的
  `--amd-runtime-dir`），所以渲染加载的就是启动时校验过的那棵树。
- **锁是包自带的清单**：`VerifyAmdRuntimeLock` 重算每个 `SHA256SUMS` 条目
  （`lmxxf-modules/`、`gfx1200|gfx1201/`、`native-game-tiled-assets/HIP/`）。
  `LmxxfNrRuntime.dll` 本身**只有存在性检查**——它不像其它模块那样有已发布的摘要，
  可信来源是获取它的那个包，不是这个函数。校验放在启动一次 + 每 job 一次。
- **渲染身份排除 `shaders/shader-cache/`**：那是 runtime 首次使用时按源码哈希
  写出来的编译产物（真正的源码 `.hlsl` 在同一棵树里、照常计入）。
  把它算进身份，会让"缓存热过没有"改变一次渲染的缓存键。
- **没有 ReShade / 没有 add-on**：`ReShade.ini`、`dxgi.dll`、feature 18 的
  log 计数器在这个后端都不存在，所以证据链换成 runtime 自己的逐帧计数
  （每捕获一帧必须至少求值一次）加上"三列 add-on 计数取反"。
  详见 `docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md` §7.1。
- **一个进程一个 job**：不服务常驻 helper 会话（`NeuralWorkerMain.cpp` 明确拒绝）。
  租约仍是按包目录派生的命名互斥量，所以两个播放器不会同时用一棵树。
- **只在源尺寸跑**：runtime 收固定盒子、按原尺寸返回，所以任何放大和
  `processing-scale != 100` 都是**拒绝**而不是近似。

上游 `lmxxf` 代码采用 MIT 许可证。`TheAutomatic/dlss-5-amd-project` 自述
GPL-3.0（基于 GPL-3.0 的 OptiScaler），**本目录只固定接口声明，不含其实现**；
自用/研究无碍，公开分发需留意 copyleft。NVIDIA 权重和第三方运行时不属于
上述任何许可证范围。

## 复现（真机 RX 9070 XT，2026-10-11）

运行时包放在仓库根的 `amd-neural-runtime/`（不进版本库）：

```text
amd-neural-runtime/
  LmxxfNrRuntime.dll          运行 DLL
  lmxxf-modules/              gfx1200/ + gfx1201/，每架构 40 个 hsaco
  native-game-tiled-assets/   权重 + shaders（经 LMXXF_WEIGHTS_DIR 传入）
  shaders/                    运行时自己的 shader 目录，与上面的同名文件内容不同
```

注意 `native-game-tiled-assets/HIP/` 是旧的 24-per-arch 子集，`Create` 会拒绝它
（`SHA256SUMS incomplete (expected 40 gfx1200 and 40 gfx1201 entries)`）；
`lmxxf-modules/` 的 40-per-arch 才是可用集合，80 条 SHA-256 全通过。

构建与运行（必须用 `env -u` 去掉大写代理变量，否则 CMake 的下载步骤会走代理）：

```bash
export MSYS_NO_PATHCONV=1
export PATH="/f/DevTools/cmake/bin:/f/DevTools/ninja:$PATH"
env -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY -u NO_PROXY \
    cmake --build build-hw --config Release --target AmdLmxxfEvaluatorSmoke --parallel

./build-hw/Release/AmdLmxxfEvaluatorSmoke.exe "" 300    # 10 s @ 1080p30，阶段 B 退出条件
./build-hw/Release/AmdLmxxfEvaluatorSmoke.exe "" 4      # 4 帧快验（run 2 会自行跳过）
```

`AmdLmxxfEvaluatorSmoke` 打 `gpu` 标签、`SKIP_RETURN_CODE 125`，所以在没有
runtime 包或没有 D3D12 适配器的机器上是"跳过"而不是"失败"。它只注册在
`DLSS_VIDEO_PLAYER_HARDWARE_TESTS=ON` 的构建里（`build-hw`）；
`build-ci` 用的 `ci` preset 把这个开关关掉，因此 300 帧那条不在 CI 里跑。

300 帧实测读数（`GetStatus`，live）：

```text
arch=gfx1201 match=luid pdl=1/1(on) modules_ok=80 hip=1
net=1920x1080 color_job=1920x1080 weights=1
recreates=0 autoexp=0 passes=1 network_passes=1 predict=1
net_gpu_ms=8.858 (frame 300)
bridgeCreates=1 releaseMarks=300 releaseMarkFailures=0
cpuMs prepare=0.28/1765.31/1765.31 enqueue=0.89/1.42/1.42 rebuild=0.00/0.00/0.00
```

- `recreates=0` 是持久输入纹理策略的直接读数：300 帧里编解码链一次都没重建。
- `prepare` 的峰值 1765 ms 只出现在**第一帧**（kernel 编译与预热），之后 0.28 ms。
  阶段 C 应把这段挪进 preflight，否则首个缓存条目的延迟会带上它。
- `releaseMarks=300` 且 `releaseMarkFailures=0`：每帧的释放标记都到位。

### 时域累积（run 5）

run 5 走的是**另一条调用路径**，因为时域模型只在 recording-lease 生命周期下
运行。lease 模式下：

- `RecordInputs` 和 `RecordOutputs` **都在 `BeginRecordingExecution` 之前**录制。
  runtime 在 job started 之后拒绝这两者，而 Begin 正是 started 的开关。
- 因此两段分别录制、分别提交，中间夹着 runtime 自己的 HIP 工作；
  `EndRecordingExecution` 在两段都提交之后**只调用一次**，`submitted_flags`
  同时带 producer 和 consumer 位，fence/value 必须是宿主自己那次覆盖了两次
  提交的 tail Signal（runtime 把它存成 job 的完成凭据，拒绝零值、`UINT64_MAX`
  或失败的 HRESULT）。
- 释放走 `InvalidateRecording` + `CollectRecording`；**`Retire` 和
  `CancelUnsubmitted` 在 lease 模式下都返回 `INVALID_ARGUMENT`**，并且
  `CollectRecording` 的 `UNAVAILABLE` 是"保留并重试"，不是失败。

源是"静止内容 + 逐帧独立噪声"（`StillNoise`），引导是恒定的零运动
（`R32G32_FLOAT`）与常量深度（`R32_FLOAT`）——**解析已知，不需要运动估计器**，
所以测到的差异只可能来自时域模型本身：

```text
lease frames through this evaluator: 24
history: active
残差高频能量   空域 6.066  →  时域 5.944   （末 8 帧均值）
与空域输出的平均绝对差: 1.548
ratio temporal/spatial: 0.980
```

**`history=active` 证明通路正确，但降噪收益只有 2%，且原因不在宿主。**
融合权重是网络自己学出来的：

```hlsl
float weight = saturate(historyStrength) * .73974609375 / (1 + exp(-logit));
```

`logit` 是网络输出的一路通道（`Network[logitOffset + i*2]`），所以"信任多少
历史"是模型的策略而非宿主的旋钮。按一阶 IIR 稳态噪声比反推，本场景下
logit ≈ −3.6，权重 **0.020**；权重的**上限是 0.7397**（稳态噪声降到 38.6%）。

另外 runtime 有一条宿主无法放宽的 disocclusion 守卫：`HistoryCompatible` 在
`any(|上一帧原始像素 − 当前帧原始像素| > 0.08)`（约 20/255）时放弃融合。
run 5 最初用 ±16 噪声（帧间差可达 31/255），大部分像素被它拦掉、收益为 0；
降到 ±8（上限 15/255）才测出上面 0.980 的真实值。

`MeanAbsoluteDifferenceRgb(produced, input)` 在 300 帧上是 3.6~5.9（0~255 尺度），
即输出确实被网络改写，且 `luminance≈124`（非黑帧）。

### 引导来自文件（run 6）

run 5 的引导是两个常量，只能回答"时域模型会不会累积"，回答不了"runtime 到底
有没有采样那张运动纹理"——静止场景下两者的正确答案都是 0。所以 evaluator 增加了
`motionGuideDirectory` / `depthGuideDirectory`：按 `src/GuideFiles.h` 的布局逐帧读
PFM（一个文件一帧，文件名是 `FrameIdentity::frameNumber`，与
`tools/benchmark --guide-dump` 写出的完全一致），`ResampleArea` 重采样到帧尺寸后
经同一张常驻纹理上传。纹理指针和尺寸仍然不动，每帧只有内容变。

run 6 在平移内容（每帧左移 8 px）上跑三条臂 × 两个噪声种子，共 6 次渲染：
A 无 lease、B lease + 真实位移场、C lease + 零位移场；源、几何、噪声抽样完全相同。

```text
guidesFromFiles=20 leaseFrames=20 history=active
同一帧、同一噪声种子下 B 与 C 的平均绝对差: 0.761
```

**B 与 C 只差运动场里的一个浮点数，输出却差了 0.761 级**——这就是"文件里的
位移向量真的进了网络"的判据，常量引导给不出这个判据。

降噪收益的度量在 run 6 里重写了一次，因为第一版量错了东西：拿"合成帧减无噪声原图"
的平均绝对差做指标，三条臂是 8.125 / 7.973 / 7.699，而噪声本身的平均幅度只有
4.0——一个什么都不做的臂反而会拿最高分。那个数在量网络对正弦内容的重塑。
正确的量法是同一臂跑两个噪声种子相减，内容与网络偏置精确抵消：

```text
源本身（不做任何降噪）        : 5.312
A  空域，无引导             : 5.325
B  lease + 真实位移场        : 5.308
C  lease + 零位移场          : 5.305
B/A 0.997   C/A 0.996   B/C 1.001
```

**在这段合成内容上时域通路几乎没有降噪收益，且与运动场对不对无关。**
与 run 5 的结论自洽：融合权重是网络学出来的 logit（本场景 0.020），不是宿主旋钮；
0.08 的 disocclusion 守卫又只放行帧间原始像素变化小于 20/255 的像素，从另一侧封顶。
现在有的是一把可信的尺子，量出的是"没有收益"而不是"量不出来"。

### 真实解码视频（run 7）

run 5 和 run 6 的内容都是测试自己发明的图案，所以两者都回答不了"这个权重是模型
的属性还是合成内容的属性"。run 7 把同一套度量搬到**真实解码的帧**上：素材是仓库
里就有的 `tools/benchmark/fixtures/demo-capture-20260912.mp4`（1920×1080 / 30 fps /
678 帧，播放器窗口的屏幕录制），解码走 `tools/fetch_ffmpeg_helpers.ps1` 落的
`external/ffmpeg/bin` 里的 ffmpeg 子进程——**就是播放器自己的解码方式**
（`VideoDecoder` 用 `CreateProcessW` 起 ffmpeg）。帧在 run 7 开头一次性解出来存在
内存里，此后所有臂渲染同一批字节，所以臂与臂之间的差别只可能来自引导。

同一个 clip 两个 subject，引导都是常量（零运动 + 单深度面）：

- `still`：每帧都用 clip 的第一帧。内容真正静止，**零运动是真值**。
- `walking`：clip 自己的连续帧。内容在动，**零场是错的**。

每个 subject 跑空域与 lease 两条路径、各两个噪声种子，共 8 次渲染。
"输入噪声"是同一估计量作用在输入自己身上的读数，即"完全不降噪"的分数。
两次运行逐位复现。

```text
                     输入噪声   空域              lease(零场)      比     隐含权重
still  (真值引导)     4.561    4.871 (1.068×)   4.490 (0.984×)  0.922   0.081
walking(错引导)       4.981    5.473 (1.099×)   5.334 (1.071×)  0.975   0.026
run 5  (合成正弦静止)      ——             ——                0.980   0.020
```

四条结论：

1. **真实纹理上的权重是 0.081，合成正弦上是 0.020，差四倍。** 权重既属于模型也
   属于内容，这个模型对合成光栅确实更不信任——所以 run 5 的 0.020 是低估，
   不能当作模型的普遍权重。
2. **但即使最有利的情况也只有 8%。** `still` 是引导全对、内容真正静止、守卫全部
   放行的最好情况，权重仍然只有 0.081。`walking` 的 0.026 是守卫把运动区域拦掉、
   只剩静止区（播放器边框、标题栏、控制栏）在融合的结果，两个 subject 的差
   正是守卫在起作用。
3. **空间路径自己在放大噪声。** 空域读数比输入高 6.8%（still）/ 9.9%（walking），
   所以降噪的基线是网络处理过的输入而不是输入；时域路径把它拉回到
   0.984× / 1.071×。拿输入当基线会把网络的放大算进时域的功劳里。
4. **收益上界是噪声标准差下降 7.8%**（`sqrt((1-0.081)/(1+0.081))`，即 0.922），
   而且这个上界落在最有利的静止内容上，真实运动场还有估计误差只会更差。
   不足以支持先写一个 CPU 块匹配器。

run 7 用到的两个路径来自编译期定义 `DLSS5_FFMPEG_PATH` / `DLSS5_REAL_CLIP_PATH`
（见 `tests/HardwareTests.cmake`），两者任一缺失时 run 7 自己跳过而不是失败。
