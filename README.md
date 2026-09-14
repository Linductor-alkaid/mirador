# Mirador

Mirador 是面向 Android、Linux 与 Windows 终端的轻量级 C++ 视觉基础设施库：把终端画面转换为
稳定、可查询、可复用的视觉事实——判断画面是否变化、按需执行 OCR/检测/几何能力、融合多源
区域证据，并在画面未显著变化时以近零成本复用结果。

Core 不链接模型 runtime、不创建线程、不绑定调度框架；平台采集、Accessibility 数据、模型执行、
VLM 调用与动作执行由调用方或独立适配层提供。详见
[设计文档](docs/design/mirador-development-design.md)与
[实施总计划](docs/plans/mirador-implementation-plan.md)。

## 模块与目标

| 目标 | 内容 | 引入里程碑 |
| --- | --- | --- |
| `mirador::core` | 基础类型、`Status`/`Result`、`ImageView`/`Frame`、坐标变换、Backend SPI | M0 |
| `mirador::image` | 颜色转换、缩放裁剪、指纹、分块差分、变化 ROI | M1 |
| `mirador::cache` | 有界缓存、能力结果缓存键、语义快照缓存、视觉索引 | M1-M3 |
| `mirador::geometry` | 线段检测（ELSED 可选）与几何过滤 | M3 |
| `mirador::fusion` | 多源证据融合、稳定 ID、generation | M4 |
| `mirador::render` | SoM 渲染与调试叠加、网格细化工具 | M4 |

当前状态（2026-09，随[实施总计划](docs/plans/mirador-implementation-plan.md)更新）：
`mirador::core`、`mirador::image`、`mirador::cache`、`mirador::geometry`、
`mirador::fusion` 与 `mirador::render` 均为编译目标。M1 落地颜色转换、裁剪、面积缩放、
dHash 指纹、分层变化检测（`detect_change`，含忽略区域与 none/partial/global 分类）
和有界 LRU `FrameCache`；M2 落地 Backend SPI（`DEC-012`：`OcrBackend`/
`DetectorBackend`、`BackendInfo` 能力与线程安全声明、`ExecutionContext` 取消通道）、
能力结果缓存 `CapabilityResultCache`（`RULE-07` 键与字节预算）和 `PerceptionSession`
（`DEC-013`：变化分析 → 按需 Backend 执行 → 坐标恢复 → 结果复用）；M3 落地
`geometry` 线段检测 SPI 与一方检测器、线段过滤（`DEC-009`）、检测/OCR 通用组件
（`DEC-014`：letterbox、NMS、类别过滤、DB 后处理、轮廓框恢复、行合并、文本规范化、
crop-refine）与有界三层视觉索引 `VisualIndex`；M4 落地确定性证据融合
（`fuse_evidence` + `FusionTrace`）、`SemanticSnapshot`/`VisualRegion` 输出模型、
跨快照稳定 ID 与 generation（`DEC-010`）、会话 `fuse()` 发布不可变快照，以及
`render` 的 Set-of-Mark 渲染与网格回映工具。可选适配器
`mirador::adapters::opencv`（`cv::Mat` ↔ `ImageView` 包装与有界导出）默认关闭，通过
`-DMIRADOR_BUILD_ADAPTERS_OPENCV=ON` 开启，要求构建环境已安装 OpenCV（不随本项目分发；
已测试 4.6.0，详见 THIRD_PARTY_NOTICES）。

## 目录结构

```text
include/mirador/        公共 API
src/core/               基础类型与状态
src/image/              图像转换、哈希与差分
src/cache/              有界缓存和视觉索引
src/geometry/           线段检测 SPI、一方检测器与几何过滤
src/fusion/             证据融合与稳定 ID
src/render/             SoM 与调试渲染
adapters/opencv/        OpenCV 类型互操作
examples/               无 runtime 的基础示例
benchmarks/             数据集与性能基准入口
tests/                  单元、属性与集成测试
```

## 构建与测试

```bash
git submodule update --init third_party/googletest   # 测试依赖（DEC-006），或 clone --recursive
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

测试通过 ctest 标签组织（`unit`/`property`/`architecture`），例如 `ctest -L architecture`
运行架构边界测试。不需要测试时可配置 `-DMIRADOR_BUILD_TESTS=OFF` 跳过第三方依赖。

## 示例与基准

```bash
cmake --build build/release --target mirador_example_change_detection
./build/release/examples/mirador_example_change_detection     # 提交帧 -> 变化检测 -> ROI/指纹 -> 帧缓存
cmake --build build/release --target mirador_example_perception_session
./build/release/examples/mirador_example_perception_session   # 会话闭环：变化分析 -> Stub OCR -> 缓存复用
cmake --build build/release --target mirador_example_road_segments
./build/release/examples/mirador_example_road_segments        # 线段检测 -> 过滤 -> 共线合并（道路边界语义）
cmake --build build/release --target mirador_example_icon_state_index
./build/release/examples/mirador_example_icon_state_index     # 图标状态入库 -> 扰动查询 -> 复用策略
cmake --build build/release --target mirador_example_hybrid_localization
./build/release/examples/mirador_example_hybrid_localization  # 混合定位：融合 -> 稳定 ID -> SoM -> generation 校验
cmake --build build/release --target mirador_bench_change_detection
./build/release/benchmarks/mirador_bench_change_detection     # 四场景 p50/p95（仅本机有效）
```

示例与基准默认随构建编译（`MIRADOR_BUILD_EXAMPLES`/`MIRADOR_BUILD_BENCHMARKS` 可关闭）；
基准数字依赖运行机器，不做跨平台比较。

预设：`debug`、`release`、`asan`、`ubsan`、`tsan`、`warnings`（Linux/macOS，Ninja）与
`windows-debug`（MSVC）；使用预设需 CMake ≥ 3.21（见 `DEC-005`）。Android NDK 交叉编译
命令见 [.github/workflows/ci.yml](.github/workflows/ci.yml)。

## 协作约定

根 [AGENTS.md](AGENTS.md) 是仓库级最高强制约束；[工程规范](docs/project/project-standards.md)
定义计划、决策、验证取证与 Git 流程。里程碑状态见
[实施总计划](docs/plans/mirador-implementation-plan.md)。

## 许可证

MIT，见 [LICENSE](LICENSE)。可选依赖与模型适配的许可证义务见
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES)与设计文档第 22 节。
