# FFX 播放帧生成白闪修复 — 真机自测报告

- **测试日期**：2026-10-16
- **测试人**：自动构建/自测（WorkBuddy agent）
- **被测分支**：`on_amd_framegen2`
- **结论**：✅ **白闪根因已定位（生成帧被二次 sRGB 编码）并修复，量化验证通过**

> ⚠️ 本报告替换了 2026-10-15 的初版。初版把「包络钳制」当作白闪主因并给出「修复有效」的结论，
> **该结论已被用户实测推翻**（包络钳制上线后用户仍报告「只要打开插帧白闪还是存在」）。
> 包络钳制本身是有效的**画质护栏**（消除边缘过冲斑点），保留，但它不是白闪的成因。

---

## 1. 结论摘要

| 项 | 修复前 | 修复后 |
|----|--------|--------|
| 生成帧视图平均亮度 | **137.33** | **92.68** |
| 正常画面平均亮度 | 92.29 | 92.29 |
| 差值 | **+45.04（比值 1.488）** | **+0.40** |
| 判定 | 生成帧比两侧源帧亮约 1.49 倍 → 交替 present 时整幅画面发白闪烁 | 与源帧一致 |

理论值 `LinearToSRGB(0.5) / 0.5 = 1.467`，实测 1.488，吻合。

---

## 2. 白闪根因：生成帧被二次 sRGB 编码

### 2.1 两套色彩域约定

播放链路里有**两个**色彩域，各自自洽，但生成帧的 present 视图把两者接错了：

1. **FidelityFX 域 = sRGB 已编码 BGRA8。**
   `m_fgFfxInput` / `m_fgFfxOutput` / `m_fgFfxClamped` 三个纹理都创建为 `DXGI_FORMAT_B8G8R8A8_UNORM`
   （FFX 要求的格式），但**内容**是 sRGB 已编码值 —— `FrameGenColorConvertShader` 在入口就做了
   `LinearToSrgb`，因为 runtime 的运动估计只在该域被验证过。

2. **backbuffer 呈现 pass = 线性光。**
   它对所有输入的约定是「采样得到线性值」，并在最后一步自己施加编码：
   `COMPOSE_ENCODE(c)` = `LinearToSRGB(c)`（SDR backbuffer）/ `max(c,0)`（HDR）。
   它平时读的 `m_dlssColor`、`m_dlssOutput` 都是 FP16 **线性**纹理，所以这个约定是对的。

### 2.2 错在哪

`CreateFrameGenResources` 里给生成帧建的 SRV 用的是 **`B8G8R8A8_UNORM`**：

```cpp
view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // ← 修复前
```

`_UNORM` 视图**不做硬件 gamma 解码**，采样器把纹理里存的值原样交给 pass。于是 pass 拿到一个
「已经是 sRGB 编码」的值，却按线性理解，再编码一次：

| 存储值（编码后） | 期望输出 | 实际输出 `LinearToSRGB(x)` |
|---|---|---|
| 0.20 (51)  | 0.20 | 0.47 (120) |
| 0.50 (128) | 0.50 | 0.71 (182) |
| 0.80 (204) | 0.80 | 0.90 (230) |
| 0.00       | 0.00 | 0.00 |

中低亮度被抬得最狠、纯黑不变 —— 电影画面的中间调占绝大多数，所以现象是**整幅画面瞬间发白**，
与用户的三条描述完全吻合：**「整幅画面瞬间发白」/ 窗口与全屏都有 / 任何内容都在闪**
（与内容无关、与会话模式无关，因为这是纯数学的亮度缩放）。

### 2.3 修复

present 链采样生成帧的两个 SRV 改为 **`DXGI_FORMAT_B8G8R8A8_UNORM_SRGB`**，由采样器完成解码：

```cpp
view.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;   // ← 修复后
m_device->CreateShaderResourceView(m_fgFfxOutput.Get(),  &view, SRVCPU(GeneratedFrameSRV));
m_device->CreateShaderResourceView(m_fgFfxClamped.Get(), &view, SRVCPU(GeneratedFrameClampedSRV));
```

这样生成帧的读入契约与 FP16 线性纹理完全一致，`COMPOSE_ENCODE` 只编码一次。

**边界刻意保持不变：**
- `m_fgClampHeap` 的三个描述符**仍是 `_UNORM`** —— 钳制在编码域比较/写入，传输单调所以包络精确；
  其 UAV 是独立视图，资源本身仍是 `_UNORM`，不违反 sRGB 不能做 UAV 的限制。
- 转换 pass 的输出域**不变**（FFX 需要编码域）。
- HDR 路径同样受益：`_SRGB` 视图先解到线性，再由 `COMPOSE_OUTPUT = LinearToPq` 输出。

契约写在 `src/FrameGenColorConvertShader.h` 的 `CONTRACT` 注释里（该文件是域的源头）。

---

## 3. 测试环境

| 项 | 值 |
|----|----|
| GPU | AMD Radeon RX 9070 XT（真机） |
| 构建 | VS 2022 + CMake（`F:/DevTools/BuildTools/.../CMake/bin/cmake.exe`），`cmake --build build-ci --config Release` |
| 产物 | `build-ci/Release/DLSSVideoPlayer.exe` → 部署 `run-fg/DLSSVideoPlayer.exe` |
| 对照版本 | `run-fg/exe-prefix.exe`（修复前，含 clamp）、`run-fg/exe-fixed.exe`（修复后） |
| 测试片源 | **`run-fg/gray-dot-fg.mkv`**（1280×720 24fps，灰 `0x808080` 底 + 缓慢移动的 80×80 亮块） |
| 验证脚本 | `run-fg/probe_srgb_domain.py`、`run-fg/verify_clamp.py` |

### 为什么用合成片源而不是大黄蜂
用电影做 A/B 时，两次采样落在**不同画面**上，亮度差被内容本身污染（初测时同一版本两次
结果差到 +43 与 +0.17）。`gray-dot-fg.mkv` 的平均亮度**任何时刻都相同**，且有一个缓慢移动的
方块使播放器持续重绘，因此两次采样严格可比。实测两次采样内部 `max == mean`（无波动），说明该片源
的均值确实恒定。

---

## 4. 测试方法

`probe_srgb_domain.py`：
1. 把 `FrameGenerationPlayback` 置 0（**这样生成帧调试视图是单次 present，能被抓屏看到**）；
2. 启动播放器，**等日志出现 `Playback health: presented=`**（播放器先做神经预渲染，早于此抓到的是欢迎页）；
3. `PostMessageW(hwnd, WM_COMMAND, IDM_VIEW_FRAME_GENERATED=306, 0)` 切到「生成帧」视图，
   **等日志出现 `island produced its first frame`** 作为切换成功的唯一可信信号
   （岛只在 `view==FrameGenerated` 或 playback FG 开时才评估）；
4. 抓 ~100 帧，算画面区平均亮度；
5. 切回 `IDM_VIEW_FINAL=302`，同样采样。

---

## 5. 测试结果

### 5.1 构建
`cmake --build build-ci --config Release --target DLSSVideoPlayer`：**0 warning / 0 error**（项目 `/W4 /WX`）。

### 5.2 色彩域 A/B（核心证据）

```
=== PRE-FIX ===
  generated mean luma 137.33  max 137.33
  final     mean luma  92.29  max  92.29
  delta     +45.04 luma

=== FIXED ===
  generated mean luma  92.68  max  92.69
  final     mean luma  92.29  max  92.29
  delta     +0.40 luma
```

- 两个版本的 **final 视图完全一致（92.29，PNG md5 相同）** → 修复只影响生成帧路径，对照成立；
- 修复前生成帧亮 1.488 倍 → 即白闪；修复后差值 0.40 / 92.29 = **0.4%**，落在测量噪声内；
- 可重复（连续两次修复后测量均为 92.68 / 92.29 / +0.40）。

### 5.3 插帧链路回归 `verify_clamp.py` → **VERIFY-RESULT: OK（6/6 PASS）**

| 检查项 | 结果 |
|--------|------|
| clamp active | ✅ PASS |
| island first frame | ✅ PASS |
| cadence active | ✅ PASS |
| no matcher fallback | ✅ PASS |
| no device removed | ✅ PASS |
| no clamp failure | ✅ PASS |

### 5.4 单元/回归测试
`ctest --preset quick`：**16/18 通过**。失败项 `PolicyTests`、`PlayerUiRegressionTests` 为
**本分支既存基线失败**（此前用 `git stash` A/B 证实，非本次引入）。

---

## 6. 过程中修正的两个错误结论（重要）

1. **「包络钳制修复了白闪」——错。** 钳制只把生成帧夹进两源帧包络，而二次编码发生在**钳制之后**
   的 present pass 里，所以钳制管不到它。钳制只消除了均值 0.08% 的边缘过冲斑点。用户实测推翻了该结论。

2. **「两个 present 被合成器合并、生成帧根本没上屏」——很可能也是错的。** 该结论基于抓屏
   发现「屏幕上只有 0/255 两个电平」，但本次发现**本机 BitBlt 抓 D3D flip-model 交换链常返回
   陈旧的合成表面**（同一次采样内帧间 diff≈0，采样之间才变）。因此该现象很可能是采集假象，
   **cadence 无需改动**；也正因如此，桌面抓屏/录屏看不到白闪，而直视屏幕（独立翻转路径）能看到。

---

## 7. 工作树改动清单

| 文件 | 状态 | 说明 |
|------|------|------|
| `src/D3D12Renderer.cpp` | 修改 | **白闪修复本体**：生成帧 present SRV 改 `_UNORM_SRGB`（1 行 + 24 行说明） |
| `src/FrameGenColorConvertShader.h` | 修改（注释） | 新增 `CONTRACT` 段，说明编码域与 present 视图必须 `_UNORM_SRGB` |
| `src/FrameGenClampShader.h` | 修改（注释） | 澄清它是画质护栏而非白闪修复；说明其描述符必须保持 `_UNORM` |
| `src/PlaybackFrameGenPolicy.h` | 新增（未跟踪） | 重建，`9f0face` 缺失文件，需与作者对齐 |
| `src/D3D12Renderer.h` | 修改 | 删除 `9f0face` 引入的重复成员声明 |
| `src/FsrFrameGenBackend.cpp` | 修改 | NaN MV 上传（保留） |
| `run-fg/probe_srgb_domain.py` | 新增 | 本报告的核心 A/B 工具 |
| `run-fg/gray-dot-fg.mkv` | 新增 | 恒定均值合成片源 |
| `run-fg/verify_clamp.py` | 修改 | 自行设置 INI 开关，不再依赖外部状态 |
| `run-fg/DLSSVideoPlayer.exe` | 已更新 | 修复后构建；对照版 `exe-prefix.exe` / `exe-fixed.exe` |

---

## 8. 结论与遗留

**白闪根因是生成帧被二次 sRGB 编码，已在真机定量验证修复**（1.488 倍 → 1.004 倍），
插帧链路健康（6/6 PASS），无测试回归。

遗留：
1. 与提交作者对齐坏提交 `9f0face`（缺 `PlaybackFrameGenPolicy.h` + 重复成员声明）。
2. `run-fg/DLSSVideoPlayer.ini` 已置 `FrameGenerationPlayback=1`（开启），可直接复测。
3. 抓屏验证手段在本机对 D3D flip-model 窗口不可靠，后续涉及「屏幕上看得到 / 抓屏看不到」
   的问题，一律改用**调试视图 + 日志证据**（生成帧调试视图是单次 present，可被抓到）。
