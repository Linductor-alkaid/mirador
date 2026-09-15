# Linux x64 基准报告：变化检测与缓存/Backend 外层开销（2026-09）

> 状态：Active（M5-06 首份数字，`M5-09` 收口时随全量报告更新）
> 日期：2026-09-15
> 负责人：linductor
> 环境：本报告全部数字按 [DEC-011](../decisions/DEC-011-benchmark-environments.md)
> 采集，**仅对下述机器与构建有效**，不构成跨平台声明。

## 环境

| 项 | 值 |
| --- | --- |
| OS | Ubuntu 24.04 x64（Linux 6.x） |
| 构建 | release（`cmake --preset release`），CMake 3.28.3 + Ninja，GCC 13.3.0 |
| commit | `feat/m5-platform-adapters-and-benchmarks` 分支（bad4822 基准入口） |
| 方法 | 确定性合成帧；预热 + 固定迭代；p50/p95 墙钟（`DEC-011` §2 口径） |

## 结果（`mirador_bench_cache_backend`，1280x720 RGBA8）

| 场景 | p50 | p95 | n |
| --- | --- | --- | --- |
| 能力缓存 raw lookup（命中） | 0.215 µs | 0.308 µs | 20000 |
| 能力缓存 raw lookup（未命中） | 0.078 µs | 0.140 µs | 20000 |
| `run_ocr` 缓存命中路径外层开销 | 2196 µs | 2263 µs | 2000 |
| `run_ocr` miss 路径外层开销（kRefresh） | 2202 µs | 2265 µs | 300 |
| 峰值 RSS（VmHWM） | 14.46 MiB | — | — |

变化检测 p50/p95 与模块体积已随 `M5-06` 收口登记于
[linux-x64-change-detection-sizes-2026-09](linux-x64-change-detection-sizes-2026-09.md)。

## 口径说明

- **命中路径不变量**：相同帧内容连续调用 `run_ocr`，Backend 恰好被调用 1 次
  （基准内置断言，非零退出即失败）——「不变画面近零成本复用」的 Core 侧证据。
- **miss 场景口径**：`cache_policy = kRefresh` 绕过缓存读，每次执行完整管线
  （指纹 → 裁剪/转换 → Backend → 坐标恢复 → 缓存写）。这是「无淘汰稳态下的
  miss 外层开销」（固定键 replace），不代表冷缓存填充动态。
- **归因**（`DEC-015` §4）：上表全部为 **Core 外层开销**；Backend 为合成桩
  （无模型成本）。runtime/模型的端到端耗时随 `M5-03`/`M5-04` 真实模型评测
  另行列报，不计入本表。
- **观察**：hit ≈ miss（差 < 1%）——外层成本由每次全帧指纹计算主导，缓存命中
  的收益体现为免去 Backend 调用本身（桩成本趋零）；真实 Backend 下的净收益
  等于其内部耗时。

## 限制（`DEC-011` 补跑条件）

- 物理 Android 设备与 Windows 桌面数字缺失；补跑前性能验收限定 Linux x64。
- CI runner 数字不采信。
- 模块静态体积已随 `M5-06` 收口登记（见变化检测/体积报告）；动态库体积与
  Android/Windows 体积随 `M5-09` 收口或补跑登记。
