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

## 目录结构

```text
include/mirador/        公共 API
src/core/               基础类型与状态
src/image/              图像转换、哈希与差分
src/cache/              有界缓存和视觉索引
src/geometry/           ELSED 等传统视觉适配
src/fusion/             证据融合与稳定 ID
src/render/             SoM 与调试渲染
adapters/opencv/        OpenCV 类型互操作
examples/               无 runtime 的基础示例
benchmarks/             数据集与性能基准入口
tests/                  单元、属性与集成测试
```

## 构建与测试

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

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
