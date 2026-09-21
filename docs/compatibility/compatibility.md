# 兼容性登记

> 状态：Active（`M5-08` 立档；M6 登记 `geometric_proposal.hpp`，`DEC-018`
> 阶段 1 转正后为正式兼容性登记；M7 新增 Experimental API 节）
> 更新日期：2026-09-21
> 负责人：linductor

本文登记 Mirador 实际验证过的构建与运行组合，以及各可选依赖的已知可用版本
区间。未列出的组合不构成兼容性承诺；性能与跨平台声明一律遵循
[DEC-011](../decisions/DEC-011-benchmark-environments.md) 的限定口径。

## 语言与构建工具

| 项 | 已验证 | 说明 |
| --- | --- | --- |
| C++ 标准 | C++20 | 公共头要求 c++20；无 C++23 依赖 |
| GCC | 13.3.0（Ubuntu 24.04） | 主开发编译器 |
| GCC（Ubuntu 20.04） | 10.5.0（focal apt `gcc-10`/`g++-10`） | CI `gcc10 / ubuntu-20.04` job（focal 容器）构建 + 测试；focal 自带 GCC 9.4 无 `std::span`，不受支持 |
| Clang | 18.1.3 | CI `clang / debug` job + fuzz/harness lint |
| MSVC | VS 2022（`windows-latest` runner 自带） | CI `msvc / ninja` job 编译 + 测试；公共头编译验证 |
| Android NDK | 26.3.11579264（arm64-v8a，android-24） | CI `ndk / arm64-v8a` job 编译 + 测试 |
| CMake | ≥ 3.16（预设需 ≥ 3.21） | `DEC-005`；本机 3.28.3 + Ninja；Ubuntu 20.04 自带 3.16.3 经 CI 验证（旧版 CMake 用 `-S`/`-B` 显式配置，无 presets；`ctest --test-dir` 需 ≥ 3.20） |
| GoogleTest | v1.18.0（pinned submodule） | 仅测试目标链接（`DEC-006`） |

## 可选依赖（integrator-provided，不随库分发）

| 依赖 | 已测试版本 | 适用区间与注意事项 |
| --- | --- | --- |
| OpenCV（仅 `adapters/opencv`） | 4.6.0（Ubuntu 24.04 apt） | 4.x 应可用：只消费 `cv::Mat` 的 data/step/type 面与 `cv::imread` 等基础面；公共 API 不出现 OpenCV 类型（`DEC-003`）。CI 用 runner 自带版本，区间以 CI 实际通过为准 |
| X11（`adapters/capture-linux`） | libx11（Ubuntu 24.04），Xorg（Xvfb）与 XWayland | 24-bit ZPixmap 视觉；XWayland root 整屏无像素后备（文档化失败语义），整屏采集需 Xorg/Wayland portal |
| Win32 GDI（`adapters/capture-windows`） | Windows Server 2022 runner（CI msvc job 编译验证） | 运行时冒烟按 `DEC-011` 待物理机补跑；分层/DRM 窗口内容不保证 |
| Android NDK media/JNI（`adapters/capture-android`） | NDK 26.3 编译验证 | AImageReader 需 API ≥ 24；真机 MediaProjection 授权流与运行冒烟待补跑 |

## Integrations（`MIRADOR_BUILD_INTEGRATIONS=ON`，默认零获取）

| 依赖 | pinned 版本 | 说明 |
| --- | --- | --- |
| ncnn | 20260526（commit e54f7b1f，`integrations/deps.lock.json`） | 参考后端专用；`DEC-015` 备选 ONNX Runtime 未启用。合成模型冒烟经 CI 验证；真实权重评测按 `RISK-2026-13` 待用户提供 |

## 公共 API 兼容性登记（`DEC-018` 阶段 1 冻结）

| 头 / 符号 | 登记依据 | 状态说明 |
| --- | --- | --- |
| `include/mirador/geometric_proposal.hpp`（`propose_regions`、`GeometricRegionProposal`、`GeometricProposalParams`、`OrientedRect`） | M6 实验轨道交付（[DEC-017](../decisions/DEC-017-geometric-region-proposal-experiment.md)）；[DEC-018](../decisions/DEC-018-geometric-region-proposal-promotion.md)（Accepted，2026-09-20）阶段 1 转正 | 自 2026-09-20 起**计入兼容性承诺**：字段与签名变更走兼容性变更流程；随 `MIRADOR_BUILD_GEOMETRY` 交付，依赖闭包仍仅标准库。`temporal_stability` 契约与融合/输出模型集成属 `DEC-018` 阶段 2，另行立项 |

## Experimental API 登记（M7，不计兼容性承诺）

| 头 / 符号 | 登记依据 | 状态说明 |
| --- | --- | --- |
| `include/mirador/object_tracker.hpp`（`TrackState`、`EvidenceGrade`、`TrackObservation`、`TrackTemplate`、`TrackSemantics`、`TargetTrack`、`ObjectTrackerOptions`、`TrackAdoption`、`ObjectTracker`） | M7-01 契约冻结（[DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md) Accepted；[跟踪设计](../design/object-tracking-design.md)） | **Experimental**：M7 内字段与签名可调整；`ObjectTracker` 随管线工作项（M7-03 起）在同一头内扩展，直至 M7-09/M7-10 go/no-go 判定后经决策冻结才计入兼容性承诺。阈值初值为开发默认值，M7-09 校准（`DEC-019` 第 5 条） |

## 平台功能可用性

| 能力 | Linux x64 | Windows x64 | Android arm64 |
| --- | --- | --- | --- |
| core/image/cache/geometry/fusion/render 全套 | ✅ 测试通过 | ✅ 编译 + 测试（CI） | ✅ 编译 + 测试（CI） |
| 采集适配 | ✅ X11 冒烟（Xvfb + XWayland 分支） | 🔨 编译验证（运行待补跑） | 🔨 编译验证（运行待补跑） |
| 参考后端（integrations） | ✅ 合成模型冒烟（CI） | ❌ 未验证 | ❌ 未验证 |
| 基准数字 | ✅ 已发布（`DEC-011` 主环境） | ⏳ 补跑条件 | ⏳ 补跑条件（功耗/温升挂起） |

✅ = 有执行证据；🔨 = 编译级验证；⏳ = 记录了补跑条件；❌ = 未验证。

## 已知行为差异与限制

- Ubuntu 20.04（focal）于 2025-05 结束标准支持：CI 以 focal 容器承载门禁，
  apt 源若迁至 old-releases 会自动改写兜底；裸 focal 容器无默认编译器，需显式
  安装 `gcc-10`/`g++-10`（GoogleTest 的 C 工程声明也要求 C 编译器）。focal 的
  OpenCV 4.2 与可选采集/integration 面未在 20.04 上验证。
- TSAN 在高熵 ASLR 内核上需 `setarch -R` 运行测试进程（CI 已内置）。
- XWayland 主机下 root 窗口捕获失败是文档化行为（`x11_capture.hpp` 契约注释），
  非缺陷；该分支由 CI xvfb job 的 Xorg 路径对偶覆盖。
- JNI `GetStringUTFChars` 为 modified UTF-8，增补字符以代理对出现（
  `adapters/capture-android/README.md` 的编码限制节）。
