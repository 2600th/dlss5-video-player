# Windows 11 开发环境准备

_适用仓库：`dlss5-video-player-on-amd`_

_适用目标：继续开发和验证 `LmxxfNrRuntime` AMD 后端。_

## 1. 先看结论

Windows 11 上需要准备四类环境：

```text
1. 播放器构建环境
   Visual Studio 2022 Build Tools / VS 2022 + MSVC v143 14.44
   Windows 11 SDK 10.0.26100
   CMake、Git、PowerShell、Python

2. Lmxxf runtime 构建环境
   MSYS2 UCRT64
   mingw-w64-ucrt-x86_64-gcc

3. 播放器源码依赖
   NVIDIA DLSS SDK 固定提交
   FFmpeg / FFprobe
   可选：yt-dlp / Deno

4. Lmxxf 运行资产
   LmxxfNrRuntime.dll
   shader
   权重
   gfx1200 / gfx1201 HSACO
   SHA256SUMS
```

如果只想编译播放器和当前接入代码，可以不安装完整 Visual Studio IDE，
使用 Visual Studio 2022 Build Tools 即可。

如果要重建 AMD HIP 模块，才需要额外的 LLVM 23.1.2 和 AMD COMGR 环境。

## 2. 推荐机器配置

```text
系统：Windows 11 x64
GPU：AMD RDNA4
  RX 9070 XT / gfx1201：首选
  RX 9060 / RX 9060 XT / gfx1200：次选
驱动：较新的 AMD Adrenalin
内存：16 GB 最低，32 GB 推荐
磁盘：至少预留 30 GB
```

驱动必须提供：

```text
amdhip64_7.dll
```

检查：

```powershell
Test-Path "$env:windir\System32\amdhip64_7.dll"
```

期望：

```text
True
```

如果结果不是 `True`：

```powershell
Get-ChildItem -Path C:\ -Filter amdhip64_7.dll -Recurse -ErrorAction SilentlyContinue |
  Select-Object -First 20 FullName
```

如果只有 `amdhip64_6.dll`，当前 Lmxxf runtime 不能使用。

## 3. 安装基础工具

推荐使用 `winget`：

```powershell
winget install --id Git.Git -e
winget install --id Kitware.CMake -e
winget install --id Python.Python.3.13 -e
winget install --id MSYS2.MSYS2 -e
```

检查：

```powershell
git --version
cmake --version
python --version
```

CMake 最低版本：

```text
3.24
```

## 4. 安装 Visual Studio 2022 Build Tools

### 4.1 为什么需要

当前播放器 CMake 明确要求：

```cmake
if(NOT WIN32)
    message(FATAL_ERROR)
endif()
if(NOT MSVC)
    message(FATAL_ERROR)
endif()
```

因此：

- 可以不用 Visual Studio IDE。
- 不能不用 MSVC 工具链。
- MinGW 不能直接构建播放器。

### 4.2 安装

可以安装：

```text
Visual Studio 2022 Build Tools
```

必须选择：

```text
Desktop development with C++
MSVC v143 x64/x86 build tools
v143 14.44 工具集
Windows 11 SDK 10.0.26100
```

当前 `CMakePresets.json` 固定：

```text
generator = Visual Studio 17 2022
toolset = v143,host=x64,version=14.44
architecture = x64
```

所以最稳妥的是完整安装 `14.44`。

### 4.3 如果只有 Visual Studio 2026

仓库的 `build_windows.bat` 可以识别：

```text
Visual Studio 17 2022
Visual Studio 18 2026
```

但 `cmake --preset vs2022` 仍会请求 VS 2022 和 v143 14.44。
只有 VS 2026 时，可改用：

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
```

正式验证仍以：

```text
Visual Studio 2022
MSVC 14.44
Windows SDK 10.0.26100
```

为准。

### 4.4 安装后检查

打开：

```text
Developer Command Prompt for VS 2022
```

或手动运行：

```powershell
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
```

确认：

```powershell
where.exe cl.exe
where.exe msbuild.exe
where.exe dumpbin.exe
cmake --version
```

至少要有：

```text
cl.exe
msbuild.exe
dumpbin.exe
```

`dumpbin.exe` 是 `ShippedRuntimeImports` 和 `PlayerDllSearchPinned`
测试需要的工具。

## 5. 安装 MSYS2 和 MinGW

### 5.1 更新 MSYS2

打开 UCRT64 终端：

```text
C:\msys64\ucrt64.exe
```

执行：

```bash
pacman -Syu
```

如果要求关闭终端，关闭后重新打开，再执行：

```bash
pacman -Su
```

### 5.2 安装 UCRT64 GCC

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-binutils
```

检查：

```bash
x86_64-w64-mingw32-g++ --version
```

Windows 路径通常是：

```text
C:\msys64\ucrt64\bin\g++.exe
```

### 5.3 构建 LmxxfNrRuntime.dll

先取得：

```text
dlss5-on-amd-9070xt-porting
```

例如：

```powershell
git clone https://github.com/lmxxf/dlss5-on-amd-9070xt-porting.git
```

建议固定已核对提交：

```text
a9b7a52165800cc4f8f98e1d5497638196413ffd
```

构建：

```powershell
cd <dlss5-on-amd-9070xt-porting>
.\scripts\build-runtime.cmd
```

如果脚本找不到 GCC：

```powershell
$env:LMXXF_GXX="C:\msys64\ucrt64\bin\g++.exe"
.\scripts\build-runtime.cmd
```

成功输出：

```text
BUILD_OK ...\LmxxfNrRuntime.dll
```

## 6. 准备播放器源码依赖

切换到目标仓库：

```powershell
cd <dlss5-video-player-on-amd>
```

### 6.1 NVIDIA DLSS SDK

即使第一期使用 AMD 后端，当前播放器 CMake 仍然要求 NVIDIA DLSS SDK。

固定提交：

```text
a291cc7d2cc642a51566f3dfd5376f635cd1b284
```

执行：

```powershell
git init external/DLSS
git -C external/DLSS remote add origin https://github.com/NVIDIA/DLSS.git
git -C external/DLSS fetch --depth 1 origin a291cc7d2cc642a51566f3dfd5376f635cd1b284
git -C external/DLSS checkout --detach FETCH_HEAD
```

检查：

```powershell
Test-Path external/DLSS/include/nvsdk_ngx.h
```

### 6.2 FFmpeg 和其他 helper

```powershell
.\tools\fetch_ui_assets.ps1
.\tools\fetch_ffmpeg_helpers.ps1
.\tools\fetch_youtube_helpers.ps1
```

检查：

```powershell
Test-Path external/ffmpeg/bin/ffmpeg.exe
Test-Path external/ffmpeg/bin/ffprobe.exe
```

如果只做设备无关测试，YouTube helper 可以稍后准备。

### 6.3 可选覆盖路径

如果依赖放在别处，CMake 支持：

```text
DLSS_SDK
FFMPEG_STAGED_DIR
YOUTUBE_STAGED_DIR
```

例如：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DDLSS_SDK=D:\deps\DLSS `
  -DFFMPEG_STAGED_DIR=D:\deps\ffmpeg\bin
```

## 7. 准备 Lmxxf 运行资产

仓库不包含完整运行资产。需要准备：

```text
LmxxfNrRuntime.dll
native-game-tiled-assets/
  native_codec_encode.hlsl
  native_codec_decode.hlsl
  其他运行时 shader
  weights/
    block0-ffn.f16 或 block0-ffn.f32
    其余权重
    noise.f32
  HIP/
    gfx1200/*.hsaco
    gfx1201/*.hsaco
    SHA256SUMS
```

建议在目标播放器仓库下建立：

```text
amd-neural-runtime/
  LmxxfNrRuntime.dll
  native-game-tiled-assets/
    ...
```

要求：

- `SHA256SUMS` 中至少能解析出 24 个 `.hsaco`。
- `gfx1201` 对应 RX 9070 系列。
- `gfx1200` 对应 RX 9060 系列。
- 权重必须与 runtime 版本匹配。
- 不要把 NVIDIA 权重或第三方 runtime 提交到 Git。

可以从作者发布的完整包取得这些资产，或按上游 `Development/` 的流程从自己
合法持有的 runtime 中提取。

## 8. 迁移当前开发成果

当前代码和测试已经有一笔本地提交：

```text
c82035d docs: 新增 AMD Lmxxf 接入方案修订版及 ABI 校验测试
```

迁移前还需要处理这两个文档改动：

```text
docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md  已修改
docs/WIN11-DEVELOPMENT-SETUP-zh.md          新增
```

推荐迁移方式：

```text
1. 在当前 Mac 上提交文档并推送到私人分支。
2. 在 Windows 11 克隆同一分支。
3. 或在保留 .git 的前提下，直接复制整个工作目录。
```

不要只复制已跟踪文件，否则未提交内容会丢失。

迁移后检查：

```powershell
git status
git log --oneline -5
```

## 9. 在 Windows 上构建播放器

### 9.1 使用官方 fixed preset

```powershell
cmake --preset vs2022
cmake --build --preset vs2022 --parallel
ctest --preset portable
```

### 9.2 使用快速 CI preset

```powershell
cmake --preset ci
cmake --build --preset ci --parallel
ctest --preset quick
```

### 9.3 只构建 AMD 新测试

配置完成后：

```powershell
cmake --build build-ci --config Release --target AmdLmxxfApiTests
ctest --test-dir build-ci -C Release -R AmdLmxxfApiTests --output-on-failure
```

如果使用 `vs2022` preset：

```powershell
cmake --build build-upscaling --config Release --target AmdLmxxfApiTests
ctest --test-dir build-upscaling -C Release -R AmdLmxxfApiTests --output-on-failure
```

## 10. 当前已实现内容

已经在仓库中落地：

```text
third_party/lmxxf/LmxxfNrApi.h
  Lmxxf C ABI 固定头

src/AmdLmxxfApi.h
  ABI、函数表、能力校验和调用阶段定义

src/AmdLmxxfPolicy.h
  RDNA4、gfx1200、gfx1201 和运行目录判定

src/AmdLmxxfSession.h
  Prepare -> RecordInputs -> EnqueueHip -> RecordOutputs -> Retire 状态机

src/AmdLmxxfRuntime.h/.cpp
  Windows 动态加载、GetApi、Create、PrepareSession 和 Destroy

tests/AmdLmxxfApiTests.cpp
  ABI、策略和调用顺序测试
```

当前 Mac 上已经通过的验证：

```text
clang++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/AmdLmxxfApiTests.cpp
AmdLmxxfApiTests: PASS

AddressSanitizer + UndefinedBehaviorSanitizer
AmdLmxxfApiTests: PASS
```

Windows 上还未完成：

```text
AmdLmxxfRuntime.cpp 的 MSVC 实编译
LmxxfNrRuntime.dll 的真实 LoadLibrary
D3D12 合成帧 API 冒烟
AmdLmxxfEvaluatorAdapter
NeuralWorker / cache / receipt 的 AMD 分支
```

## 11. 第一次 Windows 验证顺序

建议严格按这个顺序，不要一开始就改 worker：

```text
1. 编译 AmdLmxxfApiTests
2. 编译 AmdLmxxfRuntime.cpp
3. 加载固定版本 LmxxfNrRuntime.dll
4. 调用 LmxxfNrGetApi
5. QueryCapabilities
6. Create + PrepareSession
7. 创建 D3D12 device / queue
8. 上传一帧 BGRA
9. PrepareFrame + RecordInputs
10. Execute producer
11. EnqueueHip
12. RecordOutputs + readback
13. Retire
14. 保存输出并检查不是黑帧
```

只有第 14 步通过后，才继续接 `OfflineNeuralRenderer`。

## 12. 常见错误

### 12.1 `CMake Error: generator Visual Studio 17 2022 could not find`

没有安装 VS 2022。

解决：

- 安装 Visual Studio 2022 Build Tools；或
- 改用 VS 2026 generator。

### 12.2 `MSB8020`，找不到 v143 14.44

`CMakePresets.json` 固定了 `version=14.44`。

解决：

- 安装对应 MSVC 14.44 工具集；或
- 新增一个不锁版本的 preset。

### 12.3 `cl.exe` 不在 PATH

从 Developer Command Prompt 运行，或先执行：

```powershell
vcvars64.bat
```

### 12.4 `dumpbin.exe` 找不到

MSVC Build Tools 组件不完整，安装：

```text
MSVC v143 x64/x86 build tools
```

### 12.5 `LmxxfNrRuntime.dll` 不存在

先运行：

```powershell
.\scripts\build-runtime.cmd
```

或从完整包复制匹配版本的 DLL。

### 12.6 API 表校验失败

说明：

- DLL 版本不匹配；
- 头文件和 runtime 不同源；
- DLL 是旧 ABI；
- 或加载了错误文件。

必须重新固定：

```text
LmxxfNrApi.h
LmxxfNrRuntime.dll
运行资产
```

### 12.7 `SHA256SUMS missing` 或少于 24 个 HSACO

说明 `native-game-tiled-assets` 不完整。

检查：

```powershell
Get-ChildItem native-game-tiled-assets -Recurse |
  Select-Object FullName
```

### 12.8 `amdhip64_7.dll failed to load`

驱动不满足要求。

解决：

- 更新 AMD Adrenalin；
- 确认 `C:\Windows\System32\amdhip64_7.dll` 存在；
- RX 6000 不适合当前目标，Lmxxf RDNA4 路线不具备对应模块。

### 12.9 输出全黑或与原帧完全相同

检查：

- `private_output` 是否非空；
- 是否调用了 `RecordOutputs`；
- 消费者命令列表是否在执行后被正确 readback；
- 颜色格式是否是 BGRA 或 runtime 支持的格式；
- `color_width` / `color_height` 是否是有效画面尺寸，不是资源分配尺寸；
- runtime 日志中的 network geometry 和 HIP 状态。

## 13. 推荐目录结构

```text
D:\work\
  dlss5-video-player-on-amd\
    external\
      DLSS\
      ffmpeg\bin\
      youtube\
    amd-neural-runtime\
      LmxxfNrRuntime.dll
      native-game-tiled-assets\
        shaders\
        weights\
        HIP\gfx1200\
        HIP\gfx1201\
        SHA256SUMS
    build-upscaling\
    build-ci\

  dlss5-on-amd-9070xt-porting\
    src\
    hip\
    scripts\
```

推荐使用较短的纯 ASCII 路径，减少工具链和第三方脚本对路径处理的变量。

## 14. 最终检查清单

```text
[ ] Windows 11 x64
[ ] RX 9070 XT / gfx1201 或 RX 9060 / gfx1200
[ ] Adrenalin 提供 amdhip64_7.dll
[ ] Visual Studio 2022 Build Tools
[ ] MSVC v143 14.44
[ ] Windows SDK 10.0.26100
[ ] CMake 3.24+
[ ] Git
[ ] Python 3
[ ] PowerShell
[ ] MSYS2 UCRT64
[ ] mingw-w64-ucrt-x86_64-gcc
[ ] NVIDIA DLSS SDK 固定提交
[ ] FFmpeg / FFprobe
[ ] LmxxfNrRuntime.dll
[ ] shader
[ ] 权重
[ ] gfx1200 / gfx1201 HSACO
[ ] SHA256SUMS
[ ] 当前未提交开发改动已迁移
[ ] AmdLmxxfApiTests 编译通过
[ ] LmxxfNrRuntime 真实加载通过
[ ] D3D12 合成帧冒烟通过
```
