# LmxxfNrRuntime API 固定说明

来源：

```text
https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
commit a9b7a52165800cc4f8f98e1d5497638196413ffd
```

本目录只固定集成所需的 `LmxxfNrApi.h`。运行 DLL、HIP 模块、shader 和模型权重
不在本仓库内，需要由用户通过匹配的完整包或自行构建取得。

使用约束：

- 运行 DLL 必须由播放器的 AMD runtime lock 校验 SHA-256。
- API 表、帧结构和调用顺序由 `src/AmdLmxxfApi.h` 与
  `src/AmdLmxxfSession.h` 统一校验。
- 不要在业务代码中直接 `LoadLibraryW(L"LmxxfNrRuntime.dll")`；
  统一走 `src/AmdLmxxfRuntime.h`。
- 更新固定提交时必须重新核对 ABI、结构体大小、模块数量和运行锁。

上游代码采用 MIT 许可证。NVIDIA 权重和第三方运行时不属于该许可证范围。
