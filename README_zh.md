[English](README.md) | [简体中文](README_zh.md)

# Mirador

一个 C++20 库，帮程序看懂屏幕内容：判断画面是否变化、找到文字 / 控件 / 线段，并跨帧跟踪每个区域。

[![CI](https://github.com/Linductor-alkaid/mirador/actions/workflows/ci.yml/badge.svg)](https://github.com/Linductor-alkaid/mirador/actions/workflows/ci.yml)
[![Latest tag](https://img.shields.io/github/v/tag/Linductor-alkaid/mirador)](https://github.com/Linductor-alkaid/mirador/tags)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Platforms](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20Android-blue)

Mirador 位于屏幕采集代码和自动化逻辑之间：你提交帧，它告诉你哪里变了、什么东西在哪里、之前的结果还有没有效。为了省掉重复计算，它给每帧算指纹，画面没变就跳过 OCR / 检测、直接返回上次的结果；变了才执行你注入的 Backend，并把输出和你已知的区域（比如来自无障碍服务的控件位置）合成一份快照。快照里每个区域都带一个稳定 ID：按钮挪了几个像素，ID 不变，引用不会悄悄失效；变化太大就换新 ID 并递增 `generation`，拿着旧引用的代码一查便知已过期。核心库是纯 C++20，不带模型 runtime、不开线程、不联网——真正的模型通过一个很小的同步 Backend 接口由你提供。支持 Android、Linux 和 Windows。

## Features

- **变化检测** — 给每帧算指纹，分类为未变 / 局部变 / 全变；可声明忽略区域（时钟、光标、视频浮层）
- **结果缓存** — 画面没变就复用上次的 OCR / 检测结果；缓存有字节上限，键里带模型修订、ROI 和预处理版本
- **OCR / 检测 / 线段 SPI** — 几个很小的同步接口，实现由你注入（自己的 runtime，或 `integrations/` 里的 ncnn 示例）
- **证据融合** — 把 OCR 文本、检测结果、线段和你自带的区域合成一份快照
- **稳定 ID** — 区域跨帧保持 ID；大幅变化会换新 ID 并递增 `generation`，旧引用能被识别出来
- **Set-of-Mark 渲染** — 给每个区域画编号框，返回编号到 ID 的映射
- **线段与区域 Proposal** — 自研线段检测器、角度/长度过滤、共线合并、闭合形状的区域建议
- **图像工具** — 颜色转换、缩放/裁剪/letterbox、NMS、DB 后处理、文本规范化
- **视觉索引** — 按精确哈希 / 感知哈希 / 模板匹配查找图像块
- **依赖极小** — 核心只用 C++20 标准库；公共 API 不抛异常

## Quick Start

构建并运行感知会话示例（Linux/macOS；Windows 用 `windows-debug` 预设）：

```bash
git clone --recursive https://github.com/Linductor-alkaid/mirador
cd mirador
cmake --preset release
cmake --build --preset release
ctest --preset release                                          # 可选：完整测试套件
./build/release/examples/mirador_example_perception_session    # 变化 → 桩 OCR → 缓存命中 → 刷新
```

环境要求：一行式预设需要 CMake ≥ 3.21（否则用 `cmake -S . -B <dir>`，CMake ≥ 3.16），
GCC/Clang ≥ 10（libstdc++ ≥ 10）或 MSVC。默认构建只有测试需要 pinned 的 GoogleTest
子模块；`-DMIRADOR_BUILD_TESTS=OFF` 可以完全去掉这个依赖。

同样的流程写成代码——浓缩自
[`examples/perception_session_tour.cpp`](examples/perception_session_tour.cpp)，
后者是能直接构建运行、注释完整的版本：

```cpp
#include <mirador/backend_info.hpp>
#include <mirador/change_detection.hpp>
#include <mirador/execution_context.hpp>
#include <mirador/frame.hpp>
#include <mirador/geometry.hpp>
#include <mirador/image_view.hpp>
#include <mirador/ocr_backend.hpp>
#include <mirador/perception_session.hpp>
#include <mirador/pixel_format.hpp>
#include <mirador/result.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

using namespace mirador;

// 任何真实 OCR 引擎都可以通过同步 SPI 注入。这个桩实现对收到的任何视图
// 都返回一条覆盖中央区域的文本行。
class StubOcrBackend final : public OcrBackend {
public:
    BackendInfo info() const override {
        return {"stub-ocr", "1.0.0", "stub-model", "rev-1", {PixelFormat::kRgba8}, true};
    }

    Result<std::vector<TextRegion>> recognize(const ImageView& prepared, const OcrRequest&,
                                              const ExecutionContext&) override {
        TextRegion line;
        line.bounds = RectF{static_cast<float>(prepared.width) / 4.0F,
                            static_cast<float>(prepared.height) / 4.0F,
                            static_cast<float>(prepared.width) / 2.0F,
                            static_cast<float>(prepared.height) / 2.0F};
        line.utf8_text = "demo line";
        line.confidence = 0.8F;
        return std::vector<TextRegion>{std::move(line)};
    }
};

int main() {
    PerceptionSessionOptions options;
    options.source_id = "demo-screen";
    PerceptionSession session = PerceptionSession::create(options).take_value();
    StubOcrBackend ocr;

    // 一帧 320x200 的 RGBA8 图像。`owner` 在帧传递期间保持像素存活；
    // 视图本身不拥有数据。
    std::vector<std::byte> pixels(320 * 200 * 4, std::byte{0});
    Frame frame;
    frame.image.data = pixels.data();
    frame.image.width = 320;
    frame.image.height = 200;
    frame.image.row_stride_bytes = 320 * 4;
    frame.image.format = PixelFormat::kRgba8;
    frame.image.rotation = Rotation::k0;
    frame.sequence = 1;
    frame.source_id = "demo-screen";
    frame.owner = std::make_shared<const std::vector<std::byte>>(pixels);

    // 1. 变了什么？首次提交 => 一切都是新的。
    ChangeReport change = session.analyze_change(frame).value();

    // 2. 对下半屏执行 OCR；结果坐标恢复回提交时使用的坐标空间。
    OcrRequest request;
    request.roi = RectF{0.0F, 100.0F, 320.0F, 100.0F};
    std::vector<TextRegion> lines = session.run_ocr(frame, &ocr, request).value();

    // 3. 同一帧、同一请求 => 直接命中结果缓存，不再执行第二次 Backend。
    std::vector<TextRegion> cached = session.run_ocr(frame, &ocr, request).value();

    std::printf("classification=%d lines=%zu cached=%zu\n",
                static_cast<int>(change.classification), lines.size(), cached.size());
}
```

这段代码发生的事情：第一次 `analyze_change` 报告整帧都是新的；第一次 `run_ocr` 真正
执行了一次 Backend；第二次相同请求的 `run_ocr` 由缓存返回，没有再碰 Backend。

## Architecture

核心循环，从原始帧到发布结果：

```text
Frame (ImageView)
   │  提交
   ▼
变化检测 ──── 没有相关变化 ───▶ 复用上次结果
   │ 局部变 / 全变
   ▼
按需执行 Backend（OCR / 检测 / 线段，经 SPI 注入）
   │  结果映射回你的坐标系并写入缓存
   ▼
融合
   ▼
SemanticSnapshot（不可变；每个区域带稳定 ID + generation）
   ▼
Set-of-Mark 输出 / 你的代码
```

六个 CMake 库，各自可以独立开关：

| 库 | 内容 |
| --- | --- |
| `mirador::core` | 基础类型、`Status`/`Result`、`ImageView`/`Frame`、坐标变换、Backend 接口 |
| `mirador::image` | 颜色转换、缩放/裁剪/letterbox、指纹、变化检测、检测/OCR 后处理 |
| `mirador::cache` | 有界帧缓存与结果缓存、三层视觉索引 |
| `mirador::geometry` | 线段接口、自研检测器、线段过滤与共线合并、区域 Proposal |
| `mirador::fusion` | 证据融合、稳定 ID、generation、`PerceptionSession`、`SemanticSnapshot` |
| `mirador::render` | Set-of-Mark 渲染、网格划分与坐标回映 |

```text
include/mirador/        公共 API（契约写在头文件注释里）
src/core|image|cache|geometry|fusion|render/
adapters/               可选：opencv、capture-linux / -windows / -android
integrations/           基于 ncnn 的参考后端（默认构建不下载）
examples/               不依赖 runtime 的公共 API 示例
benchmarks/             性能与体积测量入口
tests/                  单元、属性、架构、并发、隐私测试与模糊入口
docs/                   设计、计划、决策、基准、API 索引、兼容性登记
```

依赖只有一个方向：可选模块和适配层建立在公共接口之上，反过来不行。OpenCV、ncnn 和
平台采集代码只出现在 `adapters/` 与 `integrations/` 里，藏在默认关闭的 CMake 选项
后面。CI 里的架构测试保证核心不带 runtime、线程和网络依赖。

## Core Concepts

| 概念 | 类型 | 是什么 |
| --- | --- | --- |
| 帧输入 | `ImageView`、`Frame` | 你手里已有像素的非拥有视图：宽高、步长、像素格式、旋转。`Frame::owner` 在帧使用期间保持内存存活。 |
| 错误处理 | `Result<T>`、`Status`、`ErrorCode` | 操作返回 `Result<T>` 而不是抛异常；每个失败都有错误码。 |
| Backend | `OcrBackend`、`DetectorBackend`、`LineDetector`、`BackendInfo` | 你实现并注入的同步接口。`BackendInfo` 声明后端身份、接受的像素格式、是否线程安全。 |
| 取消 | `ExecutionContext` | 一个取消回调加可选 deadline，传进每个长操作。超时和取消返回 `Status`，不会挂住也不会静默失败。 |
| 会话 | `PerceptionSession` | 每个图像源（窗口、屏幕、摄像头）一个。保存上一帧指纹、缓存和 ID 跟踪器。 |
| 快照 | `SemanticSnapshot`、`VisualRegion` | 一轮处理的发布结果：带边界、来源和稳定 ID 的区域集合。发布后不可变。 |
| 坐标 | `Transform2D` | 在帧、旋转后、显示三种坐标系之间变换点、矩形和线段。四种旋转、奇数尺寸、非连续步长都有测试。 |
| 缓存 | `FrameCache`、`CapabilityResultCache`、`VisualIndex` | 每个缓存都有字节上限和明确的淘汰规则。缓存键包含模型修订、参数、ROI 和预处理版本。 |

## 变化检测与缓存

`mirador::image` 给每帧计算 dHash 指纹，和上一帧逐层比较，得到分类——`none` /
`partial` / `global`——外加原因和发生变化的区域。你注册为忽略区域的地方（时钟、
闪烁的光标、视频浮层）不算变化。

- 分类为 `none`：session 跳过 Backend，直接返回上次结果。
- `FrameCache` 在字节上限内保存帧指纹和变化 ROI，按 LRU 淘汰。
- `CapabilityResultCache` 用 128 位摘要做键存储 Backend 结果，键由模型修订、模型参数、
  请求 ROI 和预处理版本算出。所以模型一升级、ROI 一变化，就绝不可能取回旧结果。
- `CachePolicy::kRefresh` 对单个请求强制重新执行一次 Backend，用于你明确知道缓存值
  不够用的场合。

## 可插拔 Backend

Mirador 不带模型。OCR、目标检测和线段检测通过 `mirador::core` 里几个很小的同步接口
接入，你针对自己用的 runtime 实现它们即可。ncnn 上的 PP-OCR 和 YOLO 系参考实现放在
`integrations/` 下，默认关闭。

- Backend 拿到的是一张已经裁剪、缩放好的视图，在视图自己的坐标系里返回结果框；
  session 负责把框映射回你提交的坐标系。letterbox、裁剪、crop-refine 都由 session
  做，不需要你的 Backend 关心。
- `BackendInfo` 声明后端的名称、版本、模型和修订、接受的像素格式、能否多线程调用。
- `ExecutionContext` 把取消和 deadline 传进长操作；被取消或超时的请求返回 `Status`，
  不会挂住，也不会静默失败。

## 融合、稳定 ID 与 Set-of-Mark

`mirador::fusion` 把 OCR 文本、检测结果、线段和你自己提供的区域（比如来自无障碍
服务）合成一份 `SemanticSnapshot`。

- 输入区域可以在帧、旋转后或显示坐标系里给出，融合前会先变换。
- 融合是确定性的，每一条接受/拒绝/合并决策都记录在 `FusionTrace` 里，事后可查。
- 每个区域带一个 `stable_id`。按钮挪动几个像素，ID 不变；区域变得认不出来，就分配
  新 ID，同时快照的 `generation` 加一。拿着旧快照引用的代码检查一下 generation，
  就知道自己手里的引用过期了。
- `mirador::render::set_of_mark` 给每个区域画编号框，返回编号到稳定 ID 的映射——
  多模态模型说"点第 3 个"，你的代码能查到那指的是哪个区域。

## 线段与区域 Proposal

`mirador::geometry` 是一个零依赖的小模块，在屏幕自动化之外也用得上：

- `SegmentGrowingLineDetector` — 从零实现的确定性线段检测器，不含第三方代码。
- `filter_segments` 和 `merge_collinear` — 按角度/长度窗口筛选线段，把共线的小段
  拼成更长的线（道路边缘、表格分隔线）。
- `propose_regions` — 把闭合和近闭合的形状组织成区域建议，按闭合度、矩形度、边缘
  支持度打分，每个建议给两个 ROI：紧贴的一个，带上下文的另一个。适合用来决定
  图标或控件搜索在哪里做。v0.3.0 起是稳定 API。

## Examples

五个示例随默认构建编译，不依赖任何模型 runtime，逐一走一遍公共 API：

| 示例 | 内容 |
| --- | --- |
| `mirador_example_change_detection` | 提交帧 → 变化检测 → ROI 与指纹 → 帧缓存 |
| `mirador_example_perception_session` | 会话循环：变化分析 → 桩 OCR → 缓存复用 → 刷新策略 |
| `mirador_example_road_segments` | 线段检测 → 角度/长度过滤 → 共线合并（道路边界语义） |
| `mirador_example_icon_state_index` | 图标状态存入视觉索引 → 带扰动查询 → 三层查找 |
| `mirador_example_hybrid_localization` | 无障碍区域 + OCR/检测融合 → 稳定 ID → Set-of-Mark → generation 校验 |

```bash
cmake --build build/release --target mirador_example_hybrid_localization
./build/release/examples/mirador_example_hybrid_localization
```

基准（`mirador_bench_change_detection`、`mirador_bench_cache_backend`、
`mirador_bench_geometric_proposal`、`mirador_bench_proposal_reuse` 和
`benchmarks/measure_sizes.sh`）在你自己的机器上测 p50/p95 延迟、缓存开销、峰值 RSS
和产物体积。已发布的数字在 [docs/benchmarks/](docs/benchmarks/)，
换一台机器就没有可比性。

## API

公共 API 就是 [`include/mirador/`](include/mirador/) 下的头文件；每个头文件自己文档化
自己的契约（语义、错误码、预算）。[docs/api/README.md](docs/api/README.md)
是模块级索引。最常用的头文件：

| 模块 | 头文件 |
| --- | --- |
| core | `status.hpp`/`result.hpp`、`image_view.hpp`/`frame.hpp`、`transform.hpp`、`ocr_backend.hpp`/`detector_backend.hpp`/`line_detector.hpp`、`backend_info.hpp`、`execution_context.hpp` |
| image | `change_detection.hpp`、`fingerprint.hpp`、`frame_cache.hpp`、`letterbox.hpp`、`crop_refine.hpp`、`detection_postprocess.hpp`、`text_postprocess.hpp`、`text_normalize.hpp` |
| cache | `cache_policy.hpp`、`capability_cache.hpp`、`visual_index.hpp` |
| geometry | `segment_growing_line_detector.hpp`、`geometric_proposal.hpp` |
| fusion | `evidence.hpp`、`fusion.hpp`、`semantic_snapshot.hpp`、`stable_id_tracker.hpp`、`perception_session.hpp` |
| render | `set_of_mark.hpp`、`grid_partition.hpp` |

在 CMake 里链接 `mirador::core`、`mirador::image`、`mirador::cache`、
`mirador::geometry`、`mirador::fusion` 或 `mirador::render`；所有符号都在
`mirador` 命名空间里。

## Platforms

| 平台 | 验证程度 |
| --- | --- |
| Linux x86-64 | CI 全矩阵（GCC 和 Clang；debug、warnings、ASAN、UBSAN、TSAN）+ Ubuntu 20.04（focal）容器任务。可选 X11 采集适配器在 Xvfb/Xorg 下有冒烟测试。 |
| Windows x86-64 | CI 里做 MSVC 编译验证（`windows-debug` 预设）；可选 GDI 采集适配器由 CI 编译。 |
| Android | CI 里做 NDK 交叉编译验证；平台无关的部分可以在任意主机构建并跑测试。MediaProjection + Accessibility 适配器由 CI 编译。 |

工具链下限：GCC/Clang ≥ 10（libstdc++ ≥ 10）、CMake ≥ 3.16（预设需 ≥ 3.21）、C++20。
核心本身没有平台相关代码。每个适配器验证到了什么程度，登记在
[docs/compatibility/compatibility.md](docs/compatibility/compatibility.md)；
基准数字只在出数的机器上有效。

## Dependencies

核心库只依赖 C++20 标准库。其余都是可选项，且不随核心分发：

| 组件 | 什么时候用到 | 许可证 | 说明 |
| --- | --- | --- | --- |
| C++20 标准库 | 始终 | — | 唯一的核心依赖 |
| GoogleTest 1.18.0 | 仅测试 | BSD-3-Clause | pinned 子模块；从不链接进库本身 |
| OpenCV 4.6.0（已测试） | `MIRADOR_BUILD_ADAPTERS_OPENCV=ON` | Apache-2.0 | 你通过 `find_package` 自行提供；只用到 `cv::Mat` 表面 |
| ncnn（pinned 20260526） | `MIRADOR_BUILD_INTEGRATIONS=ON` | BSD-3-Clause | 仅在启用时于 configure 阶段下载 |
| X11 / GDI / MediaProjection | 可选采集适配器 | 平台自带 | 不进仓库 |

模型权重不进仓库；需要真实模型的示例从命令行参数或环境变量拿路径。完整审计记录：
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES)、[dependencies.lock.json](dependencies.lock.json)
和 [docs/supply-chain/](docs/supply-chain/)。

## Roadmap

规划的六个里程碑全部完成；最新发布是
[v0.3.0](https://github.com/Linductor-alkaid/mirador/releases/tag/v0.3.0)（2026-09-20）。

| 里程碑 | 内容 | Tag |
| --- | --- | --- |
| M0 | 边界、骨架、架构测试 | `v0.1.0-alpha` |
| M1 | 基础图像、变化检测、帧缓存 | `v0.1.0-beta.1` |
| M2 | Backend SPI、结果缓存、`PerceptionSession` | `v0.1.0-beta.2` |
| M3 | 传统视觉通用组件、视觉索引、线段 | `v0.1.0-beta.3` |
| M4 | 融合、稳定 ID、Set-of-Mark | `v0.1.0` |
| M5 | 平台适配、产品化基准 | `v0.2.0` |
| M6 | 几何区域 Proposal（从实验起步） | `v0.3.0` |

暂缓的想法，每一个都在[实施总计划](docs/plans/mirador-implementation-plan.md)里写明了
触发条件：零拷贝 GPU/native buffer 路径、不破坏 ABI 的异步扩展接口、光流/特征增强的
变化检测、Embedder Backend 加嵌入向量视觉索引。只有当测量证明现有做法不够用时才会启动。

## Contributing

- `master` 是保护分支，变更经 MR 合入。Commit Message 遵循
  `<type>(<scope>): <subject>`，scope 取自 `core`、`image`、`cache`、`geometry`、
  `fusion`、`render`、`adapters`、`examples`、`benchmarks`、`tests`、`build`、`ci`。
- MR 描述要写清楚改了什么、为什么改、实际跑了哪些测试、影响什么范围。没跑过的测试不要写"通过"。
- 开发循环：`cmake --preset debug && cmake --build --preset debug && ctest --preset debug`。
  实质性变更还应通过 sanitizer 预设（`asan`、`ubsan`、`tsan`、`warnings`）；
  测试按 ctest 标签组织（`unit`、`property`、`architecture`）。
- 项目文档：[工程规范](docs/project/project-standards.md)（计划、决策、取证）、
  [AGENTS.md](AGENTS.md)（仓库规则）、[设计文档](docs/design/mirador-development-design.md)
  （系统应该怎么工作）。
- 贡献按 MIT 许可证授权。

## License

MIT——见 [LICENSE](LICENSE)。可选依赖和模型适配的许可证义务登记在
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES)。
