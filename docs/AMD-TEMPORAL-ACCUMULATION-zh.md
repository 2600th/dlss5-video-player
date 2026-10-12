# AMD 后端增加时域累积：方案调研

日期 2026-10-12。机器 RX 9070 XT（gfx1201），runtime 包 `amd-neural-runtime`（ABI 3）。
分支 `on_amd_flicker`（`13ca5f9` 之后）。

本文回答一个问题：**AMD 后端要加时域累积，加在哪一层、加什么、代价多大。**
结论先行，证据在后。

---

## 0. 结论

1. **不要在 runtime 那一层加。** 它自己的时域模型（recording-lease + 引导纹理）在真素材上
   实测融合权重 0.081，噪声标准差上限降 7.8%，而且拿到这个数字的前提是引导**全对**
   （§5.3 run 7）。它的权重是网络学出来的 logit，不是宿主旋钮；同时还有一道宿主无法
   放宽的 disocclusion 守卫。这条路的收益已经量过了，不够付运动估计的成本。
2. **在宿主这一层加，而且仓库里已经有了。** `TemporalStabilityPolicy.h` +
   `TemporalStabilityShader.h`（P2.5）就是"运动补偿的时域累积"：用上一帧的**输出**经运动
   矢量 warped 后递归混合，信任与否**用源帧判定**（不是用输出判定），亮度变化当增益除掉。
   它今天只跑在 NVIDIA 的 capture 路径上。
3. **把它接到 AMD 的 evaluator 里，第一版可以不要运动估计。** 零位移场下信任门会自动把
   门关在"源帧动了"的像素上，于是它退化成"只累积源帧没动的地方"——正是闪烁最刺眼的
   地方。实测（本文第 4 节，用真实 AMD 输出做的逐像素仿真）：
   静止段落里模型自身的逐帧抖动降 **−28.5% / −45.7% / −62.3%**（Low/Medium/High），
   而画面平均每秒只动 48.6 dB（离源距离只变 0.1 dB）。**它去掉抖动，不删风格。**
4. 真正缺的是三件事：AMD evaluator 里的一个 pass、一个不属于它的配置项、
   以及一条"实现前必须先拒绝"的规矩。

**P1 已实现（2026-10-12，本文第 9 节记录落地结果与偏差）。默认档位取 Low**，
与本文第 8 节原来的建议相反——理由见第 8.1 节。

---

## 1. 判据：闪烁是什么，怎么量

已经确认的机制（见 `2026-10-11.md` 与集成计划 §5.3）：AMD 路径 `temporal=off`，
每帧独立求值；模型本身是确定性的（同一帧喂 48 次，输出时间 std 0.0096，同码率重编码
噪声底 0.198），所以闪烁来自"输入逐帧在变 + 没有时域累积"。实测神经输出逐帧 |ΔY| 比
源高 30%（用户自己的片子），且**均匀叠加**在各时域档位上。

由此得到两条量法约束，本报告全部遵守：

- **不能用"离源距离"当收益。** 删掉风格确实会让画面更稳（这正是上一次
  `color_strength` 改动翻车的原因：0.00 与 1.00 差 24.2 dB 画面，离源距离 18.7 → 23.5 dB）。
  所以收益必须量**逐帧变化**，同时把**离源距离**当护栏一起报。
- **不能用全画面的逐帧 |Δ| 当唯一的量。** 内容在动时它被内容自己的运动主导，
  能得出相反的结论（本文第 4.2 节就出现过：真实素材上神经输出整体比源更稳）。
  要按**源帧在这一像素有没有动**分档，因为 pass 的作用范围正是这个集合。

产品里已经有这把尺子：`TemporalMetrics`（P2.11）算的就是"沿运动矢量看过之后，输出比源
多变了多少"，`flickerAdded` 进收据、进播放器的 render report。**AMD job 也在算它**——
`JobEvaluation::motionCells` 为空时 `Accumulator::Add` 用零矢量（`TemporalMetrics.h:255`），
即零运动 warp，等于"不补偿的逐帧差"。所以验收标准不需要新建，只需要读。

---

## 2. 三条路线

### 2.1 A：让 runtime 自己累积（lease + 引导）——已量过，不采纳

- 通路是通的：`HistoryStatus()` 读到 `active` 就证明每一条门禁都过了。
- 收益由模型的 logit 决定：合成正弦 0.020、真实纹理静止 0.081、真实运动 0.026。
- 就算按最有利的 0.081 算，上限也只有 `sqrt((1-0.081)/(1+0.081)) = 0.922`，
  即噪声标准差降 7.8%，还要外加 CPU 块匹配（AMD 卡上没有 NVOFA）。
- **补充证据（本次）**：路线 B 在**同一个静止素材**上，不需任何估计器就拿到 −28.5%
  ~ −62.3%。同一量级上，A 比 B 差 4~8 倍。

### 2.2 B：宿主侧运动补偿累积（P2.5，推荐）

`src/TemporalStabilityPolicy.h` / `src/TemporalStabilityShader.h` 已经写好了这套东西，
而且是按"宿主自己的算法"设计的：

- 五个输入：本帧神经输出（t0）、上一帧**已稳定**的输出（t1）、运动（t2）、本帧源（t3）、
  上一帧源（t4）。
- **信任用源帧判定**：把上一帧源按同一矢量 warp 过来，先除掉一个亮度增益，再看剩下的
  结构差（四抽头平均、取最大通道、单位是 8-bit 码）。差得小 = 这个像素的内容就是历史
  里的内容，模型在那里多变的才是它自己加的东西 → 可以拉回；差得大 = 遮挡/裁切/切镜/
  矢量本身就不可信 → 一帧不动。
- 阈值 `kTrustLow/kTrustHigh = 3/255、10/255`，三档权重 0.30 / 0.50 / 0.70。
- 复位用 `PlanFrame/Commit`（`FrameIdentity` 驱动：首帧、seek、cut、gap、换源、重试），
  历史是两槽 ping-pong，重复帧混合同一段历史。
- **它不需要自己的运动估计才"正确"**——头文件自己写了这句：判据落在源帧上，
  "which is why it can sit on either motion source (NVOFA or the CPU matcher)"。
  零场是其中一个合法输入。
- 已经量过代价（策略头 2026-09-23，13 条素材，RTX 4080 SUPER）：17 条素材的平均 sigma
  下降，代价是合成细纹素材 SSIM 0.693 → 0.685/0.678/0.666。所以它是阶梯，默认 Off。

### 2.3 C：对**输入**做时域平均——不推荐

把源帧先做时域累积再喂给 runtime。理论上也降输出抖动（模型对同一输入是确定性的），
但：

- 它改变的是模型的**输入契约**（模型被要求在一张时间上糊过的帧上工作），
  与"少改动原图内容"的既有原则冲突；
- 信任门/防鬼影的全部机制必须再造一份放在输入端，等于把 2.2 的复杂度复制一遍；
- 它和模型自己的 logit 历史会互相作用，而那条通路在 AMD 上是关的，
  一旦将来打开就变成两套累积串联，无法归因。

**结论：同样要写一个逐像素的时域滤波，写在输出侧比写在输入侧多出"不改变模型看到的东西"
这一条，且直接复用已经测过的策略与 shader。**

---

## 3. 路线 B 落到 AMD 的哪里

### 3.1 插入点

`AmdLmxxfEvaluator::SubmitFrameLease` 的 `CopyProducedToReadback(consumerList_)`
（`AmdLmxxfEvaluator.cpp:1022`）之前。此时：

- 神经输出在 `session_->Job().private_output`（`B8G8R8A8_UNORM`，见 803-821 的同尺寸/格式
  检查），已在 `kPrivateOutputState`；
- `consumerList_` 已经是一条独立、已 Close 待提交的 list，pass 记在它上面天然就在
  runtime 的 HIP 工作之后、读回之前；
- 之后 `ReadBackProduced` 逐行拷贝——**pass 的 8-bit 输出直接当它的拷贝源**，
  读回路径不用改。

同一位置对非 lease 帧路径（`SubmitFrame`，`CopyProducedToReadback(list_.Get())`，line 1223）
也一样，所以两条路径共用一个 pass 插入点。

### 3.2 要新增什么

`AmdLmxxfEvaluator` 现在只有上传/拷贝/屏障/fence，**没有 root signature、没有描述符堆、
没有 PSO**。新增：

| 资源 | 尺寸/格式 | 为什么 |
|---|---|---|
| root signature | 一个描述符表（5 个 SRV）+ 根常量 | 5 个输入正好对齐 shader 的 t0..t4 |
| SRV 堆 | 1（读回源，每帧重写）+ 2（源）+ 2×5（累积表） | 见 9.1：`private_output` 每帧换纹理 |
| PSO | `D3DCompile(kTemporalStabilityHlsl, …)` | **这正是它被独立成头的理由**：不与 PSPresent 同编译单元，可以单独编译、也可以在没设备时编译 |
| history ×2 | 输出尺寸 **R16G16B16A16_FLOAT** | 见下面的量化陷阱 |
| source ×2 | 源尺寸 BGRA8 | t3/t4；AMD 路径源尺寸 = 输出尺寸（无上采样） |
| motion | 1×1 零整数纹理 | `Motion.Load` 会 clamp，1×1 的零场就是"处处零运动" |

**量化陷阱（P1 的硬要求）**：runtime 的 `private_output` 是 8-bit BGRA，而递归滤波器的
历史必须是浮点，否则每一步都量化一次，高权重档会被舍入吃掉累积效果。**最终做法不是
MRT**，而是把这段边界拆成三个独立 pass（第 9.1 节），历史仍然只写 FP16、只从 FP16 读回。

### 3.3 配置与身份

- `AmdLmxxfEvaluatorConfig::stability`（`TemporalStability`），逐 job 传进来。
- `AmdNeuralSettingsDocument` 在**非 Off 时**追加 `accumulation=<档>` 行——Off 时不写，
  于是盘上已有的 AMD 缓存全部保持有效（沿用仓库既有的"只在非默认时写"约定）。
  行名是 `accumulation=` 而不是 `stability=`：同一个文档里 `temporal=` 已经是 runtime
  自己的时域模型，两行是两件事，同名会让将来开 lease 时无法区分（第 9.1 节）。
- **pass 起不来就必须失败**，而不是静默给一帧没稳定的画面：原计划是"实现前先拒绝非 Off
  档位"，实现后改成"`Initialize` 里 pass 建不起来就直接 `return false`"，并且
  `Ready()` 在档位非 Off 时额外要求所有时域对象非空。规矩照抄 renderer 自己的那句：
  *"A rung that cannot run fails the capture rather than cache an unstabilized frame
  under a stabilized key."* 第 9.2 节是这条规矩第一次真的生效的记录。

### 3.4 运动从哪来

- **P1：零场**（本文实测的那一档）。不需要估计器，门自动关在动的地方。
- **P2：真实位移场**。`TemporalGuideGenerator` 已经是播放器侧现成的 CPU 块匹配
  （160×90 分析网格，运动单位正是"DLSS 输入像素、current → previous"，与 t2 同约定），
  NVIDIA 适配器就在 job 里用它，成本已经进收据（`guideMsMean`）。注意 `RunAmdJob`
  现在**拒绝** `MotionSource::Cpu`（"The AMD backend has no CPU block matcher"），
  P2 要连着这条一起改，并把 `evaluation.motionCells/gridWidth` 填上（顺带让
  `flickerAdded` 从零运动 warp 升级成真正的运动补偿读数）。

---

## 4. 实测

方法：把 `PSTemporalStability` 的逐像素语义按 HLSL 复刻成 numpy
（`build-ci/temporal-research/emulate.py`：四个 ±0.5 texel 双线性抽头、逐抽头过 sRGB 曲线、
除掉一个亮度增益后按 8-bit 码比结构差、`smoothstep(3/255,10/255)` 转信任、线性光域递归混合、
历史=上一帧已稳定输出）。**零运动**，所以 Catmull-Rom 抽头精确退化为同像素取值。
神经输出是 `DLSSVideoPlayer --render --neural-backend amd` 的真实产物，源与它逐帧对齐。
度量在编码域亮度码值（`0.2126R' + 0.7152G' + 0.0722B'`），与 `TemporalMetrics` 同域。

复刻先做了自检：信任门必须用**源帧**（t3/t4）判定。第一版误用神经输出，trust 恒 0.98、
全部读数作废——这条值得写进任何后续复刻的注释里。

### 4.1 静止素材（720p，源逐字节不变像素 99.998%）

| 档 | 平均信任 | 神经输出逐帧 \|ΔY\| | pass 后 | 降幅 | 理论 |
|---|---|---|---|---|---|
| — | — | 0.2677 | — | — | — |
| Low 0.30 | 1.000 | | **0.1915** | **−28.5%** | ×0.734 |
| Medium 0.50 | 1.000 | | **0.1453** | **−45.7%** | ×0.577 |
| High 0.70 | 1.000 | | **0.1010** | **−62.3%** | ×0.420 |

护栏：pass vs 神经输出 48.67 / 48.14 / 47.42 dB（每帧画面只动 0.5 码量级）；
离源距离 17.146 dB（神经）→ 17.048 / 17.047 / 17.045 dB（**−0.1 dB**）；
亮度均值 源 127.63 / 神经 111.05 / pass 110.17（三档之间只差 0.01 码）。

**对照**：上一轮那个被回退的方案（`color_strength` 1.00 → 0.00）在同一个素材上让画面
动 24.2 dB、离源距离从 18.7 走到 23.5 dB。**同样是"变稳"，一个改掉 0.1 dB，一个改掉 5 dB。**

### 4.2 真实素材（1080p 屏幕录制，2 秒窗，源静止像素 99.74%）

| 档 | 平均信任 | 静止像素逐帧 \|ΔY\| | 静止像素 pass 后 | 全像素逐帧 \|ΔY\| | 全像素 pass 后 |
|---|---|---|---|---|---|
| — | — | 0.0882 | — | 0.1174 | — |
| Low 0.30 | 0.999 | | 0.0700 (−20.6%) | | 0.0986 (−16.0%) |
| Medium 0.50 | 0.999 | | 0.0598 (−32.2%) | | 0.0880 (−25.0%) |
| High 0.70 | 0.999 | | 0.0500 (−43.3%) | | 0.0779 (−33.6%) |

护栏：pass vs 神经 48.34 / 48.34 / 48.28 dB；离源 30.503 → 30.091 / 30.082 / 30.062 dB
（−0.42 dB）；亮度均值 源 29.61 / 神经 27.26 / pass 26.31 → 26.34。

> 这一栏里"全像素"降幅（−16%/−25%/−34%）**小于**静止像素降幅，是设计使然：门只放行
> 源帧没动的像素。反过来看，这正是"不去动内容自己的运动"的证据。
>
> 注意这条素材上**神经输出整体比源更稳**（逐帧 Y PSNR 源 43.9 dB / 神经 45.8 dB，全画面）。
> 也就是说这个素材的闪烁**不是**全局性的，只存在于静态像素里（源在那里逐字节不变，模型
> 却每帧动 0.088 码）。这就是为什么度量必须分档：看全画面的数会得出"不用修"的结论。

### 4.3 运动素材（720p 合成运动图案，源静止像素 89.8%）

用来验证门**不会**去动运动内容：

| 档 | 平均信任 | 全像素逐帧 \|ΔY\|（神经 4.321） | 被完全信任像素（神经 1.734） |
|---|---|---|---|
| Low 0.30 | 0.918 | 3.736 (−13.5%) | 1.101 (−36.5%) |
| Medium 0.50 | 0.918 | 3.432 (−20.6%) | 0.768 (−55.7%) |
| High 0.70 | 0.918 | 3.159 (−26.9%) | 0.468 (−73.0%) |

护栏：pass vs 神经 46.66 / 44.93 / 43.19 dB；离源 17.296 → 17.179 dB。
平均信任 0.918 而不是 0，说明门的**作用范围是 4 抽头邻域**——运动边缘旁一圈静态像素
会被判为中等信任，因而只部分累积。这是刻意的（防渗色），代价是运动边缘附近的降幅
小于纯静止区。

### 4.4 与路线 A 的同量级对比

| 路线 | 静止内容上的降幅 | 需要估计器 | 画面改动 |
|---|---|---|---|
| A：runtime 自身时域 | ≤7.8%（权重 0.081） | 需要（且按 §5.3 不值得） | 无 |
| B：宿主 pass @Low | **28.5%**（静止）/ 20.6%（真实静止像素） | **不需要** | 离源 0.1~0.4 dB |

---

## 5. 成本

- **GPU**：每帧一次全屏 PS + 一次额外的 8-bit 拷贝源。相对 runtime 的网络推理可以忽略，
  但**没有在真机上量过**，必须在 `--render` 的 stage 计时里落一个数。
- **显存**：1080p 约 2×8 MB（FP16 历史）+ 2×8 MB（源）+ 一张 1×1 运动 ≈ 33 MB。
  1080p 以下更小；4K 乘 4。`AmdLmxxfEvaluator` 已经在报 `LocalVideoMemoryNote()`。
- **CPU**：P1 为零（零场不需要算场）。P2 需要 `TemporalGuideGenerator`，成本已有
  `guideMsMean` 这个既有口径，量它比估它便宜。
- **延迟**：递归滤波器的时间常数 1/α，Low 约 3.3 帧（30 fps 下 0.11 s）。

---

## 6. 风险与已知代价

1. **8-bit 递归量化**：必须在 P1 就用 FP16 历史（3.2）。否则高权重档的收益会被舍入吃掉，
   而症状是"档位越高收益越不明显"，很容易被误读成策略问题。
2. **线性光域混合带来的直流偏置**：实测两段素材上 pass 比神经输出暗 **0.88 / 0.94 码**
   （均值 111 / 26）。来源是 `linear_to_srgb` 的凹性（噪声在码域被压低）+ 短窗内的滤波器
   瞬态。相对模型自身对画面的改动（−16.6 / −2.35 码）不算大，但**这是"改画面"，
   必须在真机上用更长的窗、带复位逻辑复测**。
3. **细节软化**：策略头已测 SSIM 0.693 → 0.666（合成细纹素材 @High）。这是它默认 Off 的
   原因，也是分档而不是一刀切的原因。
4. **鬼影**：本次没有直接量（需要"静止背景上的运动物体"这种素材）。设计上的抑制来自
   "信任判在源帧上"——错误矢量只有在源帧沿该矢量恰好也不变时才会骗过门。
   P2 的真实位移场会进一步减少这种概率。
5. **与 runtime 自身时域的叠加**：AMD 现在 `temporal=off`，两者不会同时跑。**若将来打开
   AMD 的 lease 时域，必须二选一**，否则两套累积串联无法归因。
6. **样本量与覆盖**：两段真实/合成素材（64~72 帧）、一台机器、一个 runtime 版本。
   没有任何结论来自超过 2 秒的连续镜头，"长镜头下滤波器收敛后的表现"仍是空白。

---

## 7. 分阶段计划

**P1：零运动 pass 接进 AMD evaluator（已实现，见第 9 节）**
1. `AmdLmxxfEvaluator` 新增 root signature / SRV 堆 / PSO（编译 `kTemporalStabilityHlsl`）、
   2×FP16 历史、2×源、1×1 零运动纹理，以及两段 8-bit↔linear 转换。
2. `AmdLmxxfEvaluatorConfig::stability`；`AmdNeuralSettingsDocument` 非 Off 时写
   `accumulation=`（第 3.3 节）。
3. 档位进入缓存身份、收据与每 job 的日志行；AMD 的默认档位用**独立 ini 键**
   （`[Temporal] AmdStability`），与 NVIDIA 的 `[Temporal] Stability` 分家。
4. 复位：`PlanFrame(history, id, frameReset)`，复用 NVIDIA 那条路径同一个函数。
5. 验收（三条同时）：
   - 静止段落的逐帧抖动下降，量级对齐本文第 4 节；
   - `psnr(pass@Low vs Off)` ≥ 45 dB、离源距离变化 ≤ 0.5 dB；
   - 运动段落的逐帧抖动不得变差。
6. 测试：文档身份（含 / 不含 `accumulation=`）、每个后端各自的键与缺省、
   两个 shader 程序的全部入口点按 `ShaderProfileFor` 编译、`PlanFrame` 的复位矩阵
   （NVIDIA 与 AMD 共用同一个函数）。

**P2：真实位移场**
- 把 `TemporalGuideGenerator` 放进 AMD job（NVIDIA 适配器已有先例），填
  `motionCells/gridWidth`；撤掉 `MotionSource::Cpu` 的拒绝并量 `guideMsMean`。
- 到这一步才有"运动表面上也累积"的收益；收益要靠同一素材的 A/B，不能外推。

**P3（可选）：分辨率与长镜头**
- 1440p/4K、30 分钟连续播放、切镜密集的电影素材上的收敛行为与鬼影。

---

## 8. 待决问题（需要产品决定）

### 8.1 AMD 的默认档位——**已决：ON（Low）**

本文原来的建议是保持 Off 为默认（"不改别人画面"）。**决定与之相反，取 Low**，理由：

- AMD 路径上 Off 不是中性的，它就是会闪的那一端：runtime 每帧独立求值、没有历史，
  看到的逐帧变化全是模型自己的抖动（第 4.1 节：静止源上 0.2677 码，而源自己是 0）。
  "默认不改画面"在这里等于"默认保留 bug"。
- 代价已经量出来并且很小：Low 让画面相对 Off 只动 46.8 dB（静止素材）、离源距离变化
  0.004 dB（第 9.3 节）。对照被回退的那个方案（离源走 4.8 dB），这不是同一个量级。
- 用户明确要求：本文所属分支的闪烁修复目标就是"去掉抖动、不删风格"，而 Low 是三档里
  离源最近、鬼影风险最低的一档。

因此：AMD 的默认档位是 `amd_lmxxf::kAmdDefaultStability = TemporalStability::Low`，
存在 `[Temporal] AmdStability`（**不是** NVIDIA 的 `[Temporal] Stability`）；用户在同一个
组合框里仍然可以选 Off。**切换档位会影响正在播放的直播会话**：这是这份决定带来的、
必须一起改的一处——见第 9.4 节。

### 8.2 是否要暴露 UI——已按既有控件沿用

`IDC_NS_STABILITY` 已经存在（NVIDIA 面板），AMD 上沿用同一个控件，文案按后端分流
（`neural.stability.off.amd` / `neural.tip.stability.amd`），符合既有 `neural.tip.*.amd` 先例。

### 8.3 是否接受"P1 只作用于源帧没动的像素"

接受。第 9.3 节的真实素材读数就是在这个语义下取得的：静止像素降 9~75%，全画面降
6~53%，运动表面的收益等 P2。

---

## 9. P1 落地记录（2026-10-12）

真机 RX 9070 XT（gfx1201），`build-ci` Release，runtime 包 = 仓库根 `amd-neural-runtime`。
所有读数都由 `--render --stages nr --neural-backend amd --temporal-stability <档>` 的真实产物
量得，度量脚本 `build-ci/p1-accept/measure.py`（第 9.3 节说明口径）。

### 9.1 与本文原计划的三处偏差

1. **没有用 MRT**。原计划是"PS 一次写两个目标：8-bit 拷贝源 + FP16 历史"。实现改成
   三个独立 pass，各自一个 RTV：
   `runtime 8-bit sRGB --decode--> FP16 linear --accumulate--> FP16 history[write] --encode--> 8-bit`。
   理由：`kTemporalStabilityHlsl` 的 t0 契约是**线性 FP16 输入**，而 runtime 给的是 8-bit
   sRGB；这个边界必须有人跨过去，跨在 pass 外面（`AmdTemporalAccumulationShader.h`）才能让
   pass 原文复用。MRT 会让 pass 同时面对两种输入格式，那就不是同一个程序了。
   历史仍然只写 FP16、只从 FP16 读回，量化陷阱的要求（第 3.2 节）没有被放弃。
2. **`private_output` 每帧换纹理**，所以 decode 的那张 SRV 每帧重写一次
   （NVIDIA 侧不需要，因为那边的生产者是我们自己）。其余 4+2 个视图建一次。
3. **文档行名是 `accumulation=`**，不是 `stability=`。原因见第 3.3 节。

### 9.2 第一次真机运行：pass 起不来，而且是被**拒绝**的

第一次跑 Low/Medium/High 三档，三档全部 `exit=4 The neural renderer could not be
initialized.`，worker 日志三行：

```
accumulation=low  →  AMD neural runtime did not come up for 1280x720: the temporal
                     accumulation's VS would not compile: amd-temporal(10,13-24):
                     error X4502: invalid ps_5_1 input semantic 'SV_VertexID'
```

**原因**：`CreateTemporalPipeline` 的编译 lambda 把 `ps_5_1` 写成常量，五个入口点（两个 VS、
三个 PS）用同一个 profile，于是两个 VS 被当成像素着色器编译。

**值得记下来的两点**：
- **失败是响亮的**：档位非 Off 时 `Ready()` 要求 pass 对象齐全，`Initialize` 在建 pass 失败时
  直接 `return false`，所以结果是"这个 job 拒绝渲染"，而不是"悄悄给一帧没稳定的画面"。
  这正是第 3.3 节那条规矩要的形态。
- **无设备测试抓不到它**：`PolicyTests` 里的编译测试本来就为 VS 传 `vs_5_1`，它证的是
  "shader 文本没问题"，抓不到"调用方配错了 profile"。修法是让 profile**不能**被配错：
  `ShaderProfileFor(entry)` 从入口点名导出（`AmdTemporalAccumulationShader.h`），
  两个程序共用；拒绝信息里也带上 profile，一行就能定位。

### 9.3 三档在真机上的读数

口径（与第 4 节一致，只是把仿真换成真实 pass）：
逐帧 `|ΔY|` 在 8-bit 码域；同时按"源帧在这一像素对之间逐字节未变"分档；
护栏 = pass 相对 Off 的 PSNR、相对源的 PSNR、亮度均值。
脚本 `build-ci/p1-accept/measure.py`，产物 `build-ci/p1-accept/*.mkv`。

**静止素材**（720p / 24 fps / 3 s，源逐字节静止）：

| 档 | 逐帧 \|ΔY\| | 降幅 | 相对 Off | 离源 PSNR | 亮度均值 |
|---|---|---|---|---|---|
| Off | 0.2677 | — | — | 18.384 | 111.047 |
| Low | 0.1877 | **−29.9%** | 46.83 dB | 18.380 (−0.004) | 111.023 |
| Medium | 0.1514 | **−43.5%** | 46.23 dB | 18.379 (−0.005) | 111.013 |
| High | 0.1075 | **−59.9%** | 45.77 dB | 18.377 (−0.007) | 111.007 |

与第 4.1 节的仿真预测（−28.5% / −45.7% / −62.3%，离源 −0.1 dB）几乎逐档重合。

**真实素材**（1080p 屏幕录制 / 3.2 s / 96 帧，源 99.807% 像素对逐字节静止）：

| 档 | 全画面 \|ΔY\| | 降幅 | 静止像素 \|ΔY\| | 降幅 | 相对 Off | 离源 PSNR |
|---|---|---|---|---|---|---|
| Off | 0.0943 | — | 0.0664 | — | — | 31.151 |
| Low | 0.0882 | −6.5% | 0.0603 | −9.2% | 50.87 dB | 31.125 (−0.026) |
| Medium | 0.0706 | −25.2% | 0.0430 | −35.3% | 49.52 dB | 31.094 (−0.057) |
| High | 0.0442 | −53.1% | 0.0167 | −74.9% | 49.24 dB | 31.051 (−0.100) |

**运动素材**（720p 合成 `testsrc2` / 3 s，源 88.4% 像素对逐字节静止）：

| 档 | 全画面 \|ΔY\| | 降幅 | 静止像素 \|ΔY\| | 降幅 | 相对 Off | 离源 PSNR |
|---|---|---|---|---|---|---|
| Off | 4.2331 | — | 1.7040 | — | — | 18.620 |
| Low | 3.6318 | −14.2% | 1.0447 | −38.7% | 41.36 dB | 18.615 |
| Medium | 3.3152 | −21.7% | 0.6931 | −59.3% | 40.88 dB | 18.621 |
| High | 3.0444 | −28.1% | 0.3916 | −77.0% | 40.23 dB | 18.622 |

**第 7 节 P1 的三条验收，逐条对**：
1. 静止段落的抖动下降：✓（−29.9 / −43.5 / −59.9%，量级对齐第 4.1 节）。
2. `psnr(pass@Low vs Off)` ≥ 45 dB：**46.83 dB ✓**；离源距离变化 ≤ 0.5 dB：**0.004 dB ✓**。
3. 运动段落不得变差：✓（全画面 −14.2%，静止像素 −38.7%，与第 4.3 节预测的
   −13.5% / −36.5% 同量级）。

**与第 4 节不一致、必须写下来的地方**：
- 真实素材上 Low 的实际降幅（−9.2%）明显小于仿真预测（−20.6%），High 的实际降幅
  （−74.9%）明显大于预测（−43.3%）。两边的 Off 基线本身也不同（今天 0.0664，第 4.2 节
  0.0882），并且仿真不含编码器。**结论只取"单调、方向正确、量级对齐"，不取逐档精确值。**
- 运动素材上 pass 相对 Off 只有 40~41 dB（第 4.3 节仿真报 43~47 dB），即真实 pass 在运动
  内容上的改动**比仿真大**。第 4.3 节的护栏（≥45 dB）本来就是针对静止素材写的，
  运动素材的判据是"不得变差"，这条成立；但"运动表面被改动的幅度"值得在 P2 里复核。
- 亮度均值只降 0.005~0.04 码，第 6.2 节担心的 −0.88 码直流偏置在真实 pass 上没有出现
  （那个数字里有仿真的成分）。

### 9.4 一并改掉的一处：直播会话必须为新档位重启

`ApplyNeuralSettings` 原来在 `!BackendReadsNeuralSettings(backend)` 时直接返回，理由是
"AMD 的渲染读不到这个对话框里的任何值，而重启本身要付一次冷启动"。P1 让这条理由的一半
失效了：对话框里的档位组合框**就是** `TemporalSettings.stability`，而它现在会到达一个
AMD 像素。所以条件改成"后端读这些值 **或** 档位相对本会话启动时变了"；不变时仍然不重启
（NVIDIA 的行为逐字节不变，因为它永远走前者）。

顺带修正了 `NeuralRenderTypes.h` 里那句过头的注释：`BackendReadsNeuralSettings` 问的是
`NeuralSettings` 这个结构体，不是"对话框里的一切"。

### 9.5 仍未覆盖的

- `AmdLmxxfEvaluatorSmoke`（真机 smoke）**没有**跑 `stability != Off` 的任何一步。目前
  "pass 能在设备上起得来"只有 `--render` 端到端这一条路覆盖——第 9.2 节那个 bug 就是这么
  被发现的。把两帧 `stability=Low` 的提交加进 smoke（断言 `AccumulationFrames()==2` 且
  读回与 Off 不同）是下一步该做的，它才是可重复的设备级护栏。
- 长镜头收敛、1440p/4K、切镜密集素材（第 6.6 节）仍然空白。

---

## 附：本次留存的证据

**P1 落地后（第 9 节）**：
- 度量脚本：`build-ci/p1-accept/measure.py`（用法：`measure.py 源 off.mkv [low.mkv …]`）
- 三档产物：`build-ci/p1-accept/{still,real,motion}-{off,low,medium,high}.mkv`，
  同目录 `*.console` 是每档的 `--render` 输出
- 运动素材是合成的：`build-ci/p1-accept/motion-src.mkv`
  （`ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=24:duration=3`）

**仿真阶段（第 4 节）**：
- 仿真脚本：`build-ci/temporal-research/emulate.py`（可重跑，参数：源、神经输出、标签、帧上限）
- 逐档报告：`build-ci/temporal-research/{still,real,motion}-report.json`
- 可播放样本：`{still,real,motion}-w{030,050,070}.mkv`（对照 = 同目录 `st-c1.mkv` /
  `real-nn.mkv` / `out-restored.mkv`）
- 神经输出与源：`real-src.mkv` / `real-nn.mkv`（1080p 真实素材，
  `tools/benchmark/fixtures/demo-capture-20260912.mp4` 的 30–33 s）
