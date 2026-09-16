# Linux x64 基准报告：几何区域 Proposal 验证 harness（2026-09）

> 状态：Active（M6-04 首份合成口径数字；`M6-06` go/no-go 判定时引用）
> 日期：2026-09-16
> 负责人：linductor
> 环境：本报告全部数字按 [DEC-011](../decisions/DEC-011-benchmark-environments.md)
> 采集，**仅对下述机器与构建有效**，不构成跨平台声明；全部为**合成场景**数字，
> 不证明真实场景价值（`RISK-2026-14`），真实截图评估按
> [evaluation-scenes](evaluation-scenes.md) 离线接入约定执行且数据不入仓。

## 环境

| 项 | 值 |
| --- | --- |
| OS | Ubuntu 24.04 x64（Linux 7.0.0-31-generic） |
| 硬件 | Intel Core Ultra 5 225H，30 GiB 内存 |
| 构建 | release（`cmake --preset release`），CMake 3.28.3 + Ninja，GCC 13.3.0 |
| commit | `feat/m6-proposal-verification-harness` 分支（基线 30069af，即 v0.2.0 收口提交） |
| 方法 | 确定性合成场景（零随机），ground truth 由生成过程给出；内置自检断言，漂移即非零退出 |

## 入口与场景

`mirador_bench_geometric_proposal`（`benchmarks/geometric_proposal_bench.cpp`，仅链接
`mirador::geometry`）在 1280x800 灰度帧上覆盖设计 §23/§24 M6 要求的五类场景，
共 21 个 GT 实体：`rect-ui`（矩形 UI，6）、`rounded-ui`（圆角，4）、
`broken-boundary`（断裂边界，4）、`decorative-frames`（装饰性框线 + GT 实体，4 GT
+ 3 装饰闭合框）、`texture-interference`（纹理干扰：494 条孤立短划线 + 3 实体）。

双口径（`RISK-2026-09` 处置，检测器缺陷不误判为假设失败）：

- **Mode A（闭合分析增益）**：生成器的精确线段直接喂给 `propose_regions`
  （默认参数），隔离闭合结构分析层。
- **Mode B（输入线段质量 / 端到端）**：场景以 1 px 硬边线段光栅化 → 一方
  `SegmentGrowingLineDetector`（默认参数，`max_segments=4096`）→
  `merge_collinear`（`distance_tolerance=3.0`，把每条 1 px 线的两道脊并回一段）
  → `propose_regions`。`line-recovery` = 被检测段按 10° 对齐、3 px 垂距覆盖
  ≥ 80% 区间的绘制线比例。

指标定义：匹配 = `tight_bounds` 与 GT 的 IoU ≥ 0.5；recall = 被覆盖 GT 比例；
precision = 匹配任一 GT 的 proposal 比例；dup-max = 单实体最多匹配 proposal 数；
tight-ROI 缩减 = 1 −（全部 proposal tight ROI 的并集像素 / 整帧像素）——按
issue #11 的"候选 ROI 实际替代整帧处理"口径，含假阳性 proposal。

## 结果：Mode A（DEC-017 晋升门槛的合成口径）

| 场景 | props | GT | covered | recall | precision | dup-max | tight-ROI 缩减 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| rect-ui | 6 | 6 | 6 | 1.000 | 1.000 | 1 | 0.638 |
| rounded-ui | 4 | 4 | 4 | 1.000 | 1.000 | 1 | 0.781 |
| broken-boundary | 4 | 4 | 4 | 1.000 | 1.000 | 1 | 0.760 |
| decorative-frames | 7 | 4 | 4 | 1.000 | 0.571 | 1 | 0.838 |
| texture-interference | 3 | 3 | 3 | 1.000 | 1.000 | 1 | 0.854 |
| **汇总** | **24** | **21** | **21** | **1.000** | **0.875** | **1** | **min 0.638** |

DEC-017 门槛（合成口径初值）：recall ≥ 0.90 **PASS**；precision ≥ 0.50
**PASS**；重复 proposal ≤ 2 个/实体 **PASS**；tight ROI 缩减 ≥ 60% **PASS**。

观察：precision 0.875 的全部折损来自装饰场景的设计负例——闭合度本身不区分
"有语义的框"与"装饰性的框"（0.571），这正是假设的预期边界：几何层只提供
弱证据，语义过滤留给 OCR/Detector/Accessibility/Fusion（issue #11、`DEC-017`
第 5 条）。

## 结果：Mode B（输入线段质量口径，非门槛口径）

| 场景 | merged | raw | line-recovery | recall | precision | dup-max | tight-ROI 缩减 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| rect-ui | 24 | 48 | 1.000 | 1.000 | 1.000 | 1 | 0.634 |
| rounded-ui | 16 | 32 | 0.333 | 0.000 | 1.000 | 0 | 1.000 |
| broken-boundary | 20 | 40 | 1.000 | 1.000 | 1.000 | 1 | 0.757 |
| decorative-frames | 29 | 60 | 1.000 | 1.000 | 0.571 | 1 | 0.836 |
| texture-interference | 506 | 1012 | 1.000 | 1.000 | 1.000 | 1 | 0.852 |

- `rounded-ui` 是唯一的端到端失败点，机制已定位：本 harness 采用硬边（无抗
  锯齿）光栅化，非轴对齐短弧弦的中央差分梯度方向随楼梯步进震荡（垂直 ↔ 对角
  交替，相差 45°），超出检测器 15° 区域生长容差，约 36 px 的双弦弧只恢复出
  ~8 px 碎段，四角均断裂后闭合度按定义趋近 0（`1 - gap/diagonal`），闭合层
  行为正确。按 `RISK-2026-09` 处置，这记入**输入线段质量**口径，不作为假设
  失败证据；闭合层对圆角的能力由 Mode A（recall 1.000）单独证明。真实截图为
  抗锯齿内容，属真实数据口径，按离线约定另行评估。
- 其余四类场景端到端 recall 与 Mode A 一致；抗断裂（gap 24~44 px 未被
  `merge_collinear` 的 4 px 容差桥接）与抗纹理（494 条互不接触的短划线零
  proposal、零预算压力）均按预期。

## 耗时随线段数曲线（`RISK-2026-15`）

Mode A 生成器（8 个闭合框 + 孤立填充短划线），`propose_regions` 默认参数，
300 次迭代 + 20 次预热，p50/p95 墙钟：

| 输入线段数 | p50 | p95 |
| --- | --- | --- |
| 64 | 25.3 µs | 25.9 µs |
| 128 | 61.7 µs | 75.0 µs |
| 256 | 200.0 µs | 208.4 µs |
| 512 | 583.5 µs | 621.6 µs |
| 1024 | 2290.0 µs | 2329.5 µs |

耗时随线段数按超线性（接近二次）增长，与端点邻近链接的已知复杂度一致；
1024 段（默认预算上限）p50 ≈ 2.3 ms，仍远低于单帧预算，`kBudgetExceeded`
上限是主要防护而非耗时。

## Temporal 占位（信息性，无门槛）

同一 rect-ui 场景整体平移 −2/+2 px 的 5 帧合成序列：每帧 proposal 数恒为 6，
逐实体 tight-ROI IoU 最差值 1.000，数量稳定。整体平移不改变结构间关系，该
占位只证明确定性重渲染下的自洽性；跨帧关联与 `temporal_stability` 的正式
契约在 `M6-05`（条件触发）建立。

## 口径限定与复现

- **合成 ≠ 真实**（`RISK-2026-14`）：本页数字只证明"闭合结构先验在合成场景
  可实现且满足 `DEC-017` 初值门槛"；真实截图价值待离线数据评估，go/no-go
  判定（`M6-06`）必须携带该限定。
- **环境限定**（`DEC-011`）：数字仅对本页四元组有效；CI runner 数字不采信；
  Android/Windows 不作结论。
- **指标口径**：匹配阈值 IoU 0.5、缩减含全部 proposal、装饰负例计入
  precision，均为 harness 内置定义，随入口代码固定，改动需换新场景版本号。
- 复现命令：`cmake --preset release && cmake --build build/release --target
  mirador_bench_geometric_proposal && ./build/release/benchmarks/mirador_bench_geometric_proposal`。
