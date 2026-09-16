# benchmarks

数据集组织与性能基准入口：变化检测 p50/p95、缓存命中路径延迟、Backend 调用外层耗时、
RSS 与体积。性能验收必须发布基准环境与数字（设计文档 §20、§26）；结果记录在
`docs/benchmarks/`。

基准是可执行文件而非 ctest 用例：CI 只验证其构建，数字只在运行它的机器上有效
（`DEC-011` 基准环境与方法）。

| 入口 | 覆盖 | 说明 |
| --- | --- | --- |
| `mirador_bench_change_detection` | 变化检测 p50/p95（不变画面、局部变化、旋转、动态区域忽略） | M1-08；数字见 [linux-x64-change-detection-sizes-2026-09](../docs/benchmarks/linux-x64-change-detection-sizes-2026-09.md) |
| `mirador_bench_cache_backend` | 能力缓存查找 hit/miss、`run_ocr` 缓存命中外层开销（同帧不重复调用 Backend）、`kRefresh` 完整 miss 路径外层开销、峰值 RSS | M5-06；数字见 [linux-x64-cache-backend-2026-09](../docs/benchmarks/linux-x64-cache-backend-2026-09.md) |
| `mirador_bench_geometric_proposal` | 实验性几何区域 Proposal 的合成验证：五类场景 recall/precision/重复率/ROI 缩减（DEC-017 门槛）、端到端双口径（`RISK-2026-09`）、耗时随线段数曲线（`RISK-2026-15`）、temporal 占位 | M6-04；数字见 [linux-x64-geometric-proposal-2026-09](../docs/benchmarks/linux-x64-geometric-proposal-2026-09.md) |
| `measure_sizes.sh <build-dir>` | 模块静态库与可执行文件体积表 | M5-06；数字见 [linux-x64-change-detection-sizes-2026-09](../docs/benchmarks/linux-x64-change-detection-sizes-2026-09.md) |

评测集场景清单与离线数据接入约定见
[docs/benchmarks/evaluation-scenes.md](../docs/benchmarks/evaluation-scenes.md)；
约定：确定性合成帧（无随机）、预热 + 固定迭代、p50/p95 墙钟；归因口径见
`DEC-015` 第 4 节（Core 外层与 runtime 内部耗时分开列报）。
