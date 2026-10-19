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
| `mirador_bench_proposal_reuse` | 实验性几何 Proposal 的跨帧关联稳定性探针（8 实体 × 6 帧 ±2 px 抖动）与 Geometry-gated VisualIndex 命中对照（distinct/ambiguous 双变体） | M6-05；数字见 [linux-x64-geometric-proposal-reuse-2026-09](../docs/benchmarks/linux-x64-geometric-proposal-reuse-2026-09.md) |
| `mirador_bench_object_tracking` | 跨帧目标跟踪合成验证 harness：A/B/C/D 方法 × 六场景（`*-static-page`/`*-scroll`/`*-dialog`/`*-theme-switch`/`*-similar-icons`/`*-partial-anim`）× §8 全套指标与门槛初值校准证据；oracle Detector 粗召回，kLost 静止零触发 / 池预算 / 逐位确定性内建断言 | M7-09；数字见 [linux-x64-object-tracking-2026-09](../docs/benchmarks/linux-x64-object-tracking-2026-09.md) |
| `mirador_bench_desktop_perception [P6-file [iterations]]` | E1 验证、变化检测同帧/变帧、2px 描边；默认确定性 400×240 合成 RGB，外部 P6 用 98×27 的固定桌面 ROI（55,530），需包含该区域 | DP-01；[方法、结果与限制](../docs/benchmarks/linux-x64-desktop-perception-2026-10.md) |
| `mirador_bench_ocr_budget` | 交互 OCR 预算 harness：确定性合成密集/稀疏/长行场景 × baseline 全量与 budgeted 有界路径，阶段耗时分解与逐调用无内容 CSV；权重经显式路径，缺失 exit 3 | M8-01；方法与门槛见 [OCR 预算协议](../docs/benchmarks/ocr-budget-protocol.md)，真实权重实测待 M8-06 |
| `mirador_bench_ocr_rec_experiment` | M8-05 rec 优化实验（真实权重、调用方路径）：单行成本分解、动态/固定/分桶宽度策略、4 行横向打包与 ncnn 线程扩展八臂 + 可选 dense 端到端对照；跨次确定性门、逐单元 CSV，语料由 `tools/render_ocr_corpus.py` 确定性生成 | M8-05；方法、数字与 adopt/no-go 结论见 [rec 优化实验报告](../docs/benchmarks/ocr-rec-opt-experiment-2026-10.md) |
| `measure_sizes.sh <build-dir>` | 模块静态库与可执行文件体积表 | M5-06；数字见 [linux-x64-change-detection-sizes-2026-09](../docs/benchmarks/linux-x64-change-detection-sizes-2026-09.md) |

评测集场景清单与离线数据接入约定见
[docs/benchmarks/evaluation-scenes.md](../docs/benchmarks/evaluation-scenes.md)；
约定：确定性合成帧（无随机）、预热 + 固定迭代、p50/p95 墙钟；归因口径见
`DEC-015` 第 4 节（Core 外层与 runtime 内部耗时分开列报）。
