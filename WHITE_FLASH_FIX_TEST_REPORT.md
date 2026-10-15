# FFX 播放帧生成白闪修复 — 真机自测报告

- **测试日期**：2026-10-15
- **测试人**：自动构建/自测（WorkBuddy agent）
- **被测分支**：`on_amd_framegen2`（含刚拉的 `9f0face` + 未提交白闪修复改动）
- **结论**：✅ 白闪修复在真机生效且健康，修复有效

---

## 1. 测试目的

验证 FFX 播放帧生成「白闪」修复在 AMD 真机上：
1. 能正常构建并运行；
2. 包络钳制（envelope clamp）已激活，生成帧被约束进源帧包络；
3. 帧生成 island 模式正常出帧、双 present cadence 正常生效、无降级；
4. 量化验证白闪指标 `overBoth`（比两源帧都亮的像素占比）归零或仅残余极低。

### 白闪根因与修复简述
- **白闪定义**：「生成帧超包络斑点」——无运动矢量（MV）时，FFX 色彩一致性校正（color consistency）在高对比边缘过冲（overBoth≈1% 像素比两源帧亮，约 0.6% 帧率出现，24Hz 交替→肉眼闪烁；录屏/抓屏因采样+编码抓不到）。
- **修复**（未提交，位于 `src/FrameGenClampShader.h` 等）：
  - 上传 **NaN MV**（`UploadGuides motionClear=NaN`，语义为「无数据」而非「静止」；F 帧 4→2）；
  - **包络钳制**：生成帧每个像素钳进 `[min(prev,cur), max(prev,cur)]`，构造性消除 overBoth。

---

## 2. 测试环境

| 项 | 值 |
|----|----|
| GPU | AMD Radeon RX 9070 XT（真机） |
| 构建工具链 | VS 2022 BuildTools + CMake/Ninja（经 `vcvars64.bat` 注入 INCLUDE/LIB） |
| 构建产物 | `build-ninja/DLSSVideoPlayer.exe`（含修复，链接成功） |
| 运行时 | `run-fg/DLSSVideoPlayer.exe`（已部署新构建，旧版备份 `DLSSVideoPlayer.exe.bak-preclamp`） |
| 测试视频 | `M:\电影\阳光电影www.ygdy8.com.大黄蜂.BD.720p.中英双字幕.mkv` |
| probe 帧集 | `build-ninja/probe/frames/bmb-scan.rgba`（337 帧 1280×720 BGRA） |
| 验证脚本 | `run-fg/verify_clamp.py`、 `build-ninja/probe/probe-scan.exe` |

---

## 3. 测试前置问题（重要，属拦路 bug，非修复问题）

刚拉的提交 **`9f0face`（fix: 修正播放帧生成状态上报）是坏提交**，player 直接编译不过，必须先修才能自测：

1. **缺失头文件**：`src/D3D12Renderer.h:16` 引用 `#include "PlaybackFrameGenPolicy.h"`，但该头文件从未被提交（git 全历史、全盘均无，作者漏 `git add`）。
2. **重复成员声明**：同一文件 `m_fgResetPending` / `m_fgDoublingActive` 各声明两次（C2086 硬错误），亦为该提交引入。

**处理**：按精确调用点（花括号初始化字段顺序取自 `D3D12Renderer.h:671/683`，语义取自 `PolicyTests` 断言）重建 `src/PlaybackFrameGenPolicy.h`（`namespace playback_frame_gen`：`RuntimeStatus{requested,islandUp,failed,doublingPresented}` + `RuntimeAvailable`/`DoublingActive`/`ResetHistory`），并删除 `D3D12Renderer.h` 中重复的两行声明。构建通过。

> ⚠️ 该头文件属 `9f0face` 缺失文件，**建议与原提交人（qianyu.wang）对齐后再决定是否纳入提交**。

---

## 4. 测试方法

- **方法 A（运行时验证）**：`verify_clamp.py` 启动播放器（FG=on），播放数秒，对日志做 6 项状态断言（clamp active / island first frame / cadence active / no matcher fallback / no device removed / no clamp failure）。
- **方法 B（构造性证明）**：直接读 `FrameGenClampShader.h`，确认钳制把生成帧每个像素夹进 `[min(prev,cur), max(prev,cur)]`（sRGB 单调域可直接钳）。白闪指标 `overBoth` 定义即「比两源帧都亮的像素」，故钳制生效时 `overBoth ≡ 0` 为数学必然，非概率改善。
- **方法 C（量化标尺）**：`probe-scan` 用当前后端对 `bmb-scan.rgba` 跑帧生成，统计 `overBoth` 实际峰值。

---

## 5. 测试结果

### 5.1 构建
- `cmake --build build-ninja --target DLSSVideoPlayer`：**0 error / 0 FAILED**，exe 链接成功。

### 5.2 运行时验证 `verify_clamp.py` → **VERIFY-RESULT: OK（6/6 PASS）**

| 检查项 | 结果 | 日志证据（来自 `DLSSVideoPlayer.log`，时间戳 15:29:58） |
|--------|------|------|
| clamp active | ✅ PASS | `Playback frame generation: the envelope clamp is active - generated frames are held to the min/max of their two source frames.` |
| island first frame | ✅ PASS | `Playback frame generation: the island produced its first frame (1280x720, interval 41.7083 ms).` |
| cadence active | ✅ PASS | `Playback frame generation: cadence active - each source frame is presented as the generated midpoint and then the frame itself.` |
| no matcher fallback | ✅ PASS | 日志无 `falling back to the matcher` |
| no device removed | ✅ PASS | 日志无 `device removed`（FFX Initialize 后 `removedReason=0x00000000`） |
| no clamp failure | ✅ PASS | 日志无 `clamp shader did not compile` / `clamp pipeline did not build` / `clamp textures could not` |

### 5.3 构造性保证
`FrameGenClampShader.h` 将生成帧每个像素钳进 `[min(prev,cur), max(prev,cur)]`（sRGB 域单调）。白闪的量化指标 `overBoth`（比两源帧都亮的像素）在钳制生效时**恒为 0**，属构造性消除。

### 5.4 量化标尺 `probe-scan`（当前修复后端重编，`bmb-scan.rgba`）
```
[done] frames=337 generated=329 cuts=7 flagged=0
```
- `overBoth`：绝大多数为 `0.00%`，高对比帧**峰值 0.44%**（后端级残余，present 链 clamp 再清零）。
- `flagged=0`：无帧触发白闪告警阈值。

---

## 6. 测试中发现的不稳定项（已排除，非修复问题）

1. **验证脚本配置漂移（已复测排除）**：首次自测时发现 `verify_clamp.py` 三项断言失败（clamp active / island first frame / cadence active）。排查确认 `run-fg/DLSSVideoPlayer.ini` 中 `FrameGenerationPlayback` 被置 `0`、`[FrameGeneration] Playback` 缺失 → 播放器未进 island 模式。将 FG 开关恢复为 `1` 后**复测 6/6 全 PASS**。结论：属测试配置文件漂移，与白闪修复无关。
2. **截图式闪烁计数不可用（本会话）**：`repro_white_flicker.py` 用 `ImageGrab` 抓取窗口，在本会话抓到的是 WorkBuddy 界面而非播放器视频区，FG-on/off 均报 100% white（97/97、48/48），属采集 artifact，无法作为判别证据。该脚本需播放器窗口处于前台才能使用，**未纳入正式结论**。

---

## 7. 工作树改动清单

| 文件 | 状态 | 说明 |
|------|------|------|
| `src/PlaybackFrameGenPolicy.h` | 新增（未跟踪） | 重建，9f0face 缺失文件，需与作者对齐 |
| `src/D3D12Renderer.h` | 修改 | 删除 `9f0face` 引入的重复成员声明 |
| `src/FrameGenClampShader.h` | 未提交改动 | 白闪修复本体（包络钳制 shader） |
| `src/D3D12Renderer.cpp` | 未提交改动 | 白闪修复（clamp 纹理 / 采样 / present 链） |
| `src/FsrFrameGenBackend.cpp` | 未提交改动 | 白闪修复（NaN MV 上传） |
| `run-fg/DLSSVideoPlayer.exe` | 已更新 | 含修复的新构建；旧版备份 `DLSSVideoPlayer.exe.bak-preclamp` |

---

## 8. 结论

白闪修复在真机（AMD RX 9070 XT）上**通过自测**：
- 构建成功（0 error）；
- 包络钳制已激活、island 正常出帧、双 present cadence 生效、无降级、无设备移除、无 shader/管线/纹理失败；
- 白闪指标 `overBoth` 由构造性钳制归零（峰值仅 0.44% 后端级残余，`flagged=0`）。

**修复有效。** 遗留两项：① 与提交作者对齐坏提交 `9f0face`（缺头文件 + 重复声明）；② 截图式闪烁计数脚本需播放器前台窗口方能使用。
