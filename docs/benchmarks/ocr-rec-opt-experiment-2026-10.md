# M8-05：rec 优化实验报告——单行成本、宽度策略、packing 与 batching

> 状态：Delivered（adopt/no-go 结论齐备；矩阵证据由 Independent-Verification-Agent
> 独立构建与执行）
> 日期：2026-10-19
> 决策依据：[DEC-025](../decisions/DEC-025-budgeted-ocr-and-partial-results.md) 第 6 节、
> [M8 计划](../plans/m8-budgeted-ocr.md) AC8-09
> 工具：`benchmarks/ocr_rec_experiment.cpp` + `benchmarks/tools/render_ocr_corpus.py`
> （语料与权重均为调用方路径，不入仓）

## 1. 问题与结论

Issue #41 的交互失败来自逐行串行 rec：密集帧 359–512 个候选行意味着同等数量的
forward。M8-01…04 已交付有界执行（top-K、优先级、软预算、部分报告）；本实验回答
DEC-025 第 6 节预留的问题——**是否存在值得采纳的 rec 单元级优化**（单行成本结构、
动态/固定/分桶宽度、横向 packing、线程扩展、batching），并按 AC8-09 门槛给出
adopt/no-go。

**结论（全部 no-go，不阻塞 M8-06）：**

| 候选 | 结论 | 一句话理由 |
| --- | --- | --- |
| 真 batching | **no-go（能力性）** | pinned rec 计算图 Input 为 3 维（无 batch 轴），pinned ncnn `Extractor`/`Mat` 执行模型无样本维；伪 batch 不计（AC8-09） |
| 横向 packing（4 行拼接） | **no-go** | 组级文本匹配率 8.3%–20.8%（与频率环境无关）；解除宽度上限后同样内容 wall 1.72 倍、RSS 4.9 倍（509 MB） |
| 固定宽 pad320 | **no-go** | 文本一致率 82.3%、置信度最大差 0.62——质量否决与频率环境无关；动态宽度设计得到证据支持 |
| 宽度分桶 bucket32/64 | **no-go** | 文本一致率 96.9%/90.6% 违反 AC8-09"文本一致"硬门槛；padding 改变 CTC 时间步，输出差异无法压到门槛内，机制上不可修复 |
| ncnn 线程扩展（threads2/4） | **no-go（维持 `num_threads=1` 默认）** | ABBA 顺序对照显示暖机 serial（p50 32.9 ms / p95 35.7 ms）反而比 threads4 快 12.8–23.5%；主矩阵的表面收益被证实为 powersave 冷启动顺序伪影。与 DEC-012 确定性契约及 DEC-025 备选方案"线程/绑核已报告负优化"一致，无任何变更 |

## 2. 测量环境与方法

- 机器：Intel Core Ultra 5 225H（14 核混合：4P 4.9 GHz + 8E 4.4 GHz + 2LP-E 2.5 GHz），
  `taskset -c 0-3`（P 核）绑定，governor powersave（未调频，如实记录），内核
  7.0.0-38-generic，gcc 13.3.0 Release 构建（`-DCMAKE_BUILD_TYPE=Release`）。
- Runtime：pinned ncnn `e54f7b1f88434e1d844ea0551b880a1cfb079ce1`（20260526，
  BSD-3-Clause），`use_packing_layout=false`（M5 张量面契约，见 §6 局限）。
- 权重：Mirage 侧适配的 `PP-OCRv5-mobile-ncnn-mirador-contract-v1`（det/rec/charset
  五文件 SHA-256 与 Mirage 供应链审计 `real-ocr-resources-20261009.md` 逐一相符）。
- 语料：确定性生成 96 行桌面风格中英文文本（Noto Sans CJK SC Regular，ttc index 2，
  seed 20261019），四个宽度类各 24 行；两次生成逐字节一致（`diff -r` 无输出）。
  dense 帧 1600×1000、338 文本块。
- 方法：每臂独立 ncnn runtime 实例，warmup 5 轮后计量 20 轮 × 96 单元（pack 臂
  20 × 24 组）；逐单元 steady_clock 墙钟 + 阶段分解（preprocess/forward/decode）；
  跨次确定性门（threads=1 臂硬门、threads>1 臂报告式）；serial 臂同时作为质量
  基线（文本逐字节相等 + 置信度最大绝对差）。CSV 逐单元无内容（无图像字节）。
- 执行者：Independent-Verification-Agent 独立完成构建、语料、计量与交叉验证
  （从 units.csv 重算 p50/p95 与 summary 逐位一致）。

## 3. 单行成本分解（serial 基线）

主矩阵 serial 臂（最先运行，数字含 powersave 冷启动尾，仅作量级参考）：

| 宽度类 | 行数 | resize 后平均宽 | p50 | p95 |
| --- | --- | --- | --- | --- |
| short | 24 | 87.0 px | 19.8 ms | 59.2 ms |
| medium | 24 | 231.1 px | 47.7 ms | 149.2 ms |
| long（触 320 上限） | 24 | 320 px | 66.7 ms | 155.3 ms |
| overlong（被压到 320） | 24 | 320 px | 66.8 ms | 166.7 ms |

阶段占比：preprocess 0.4%、**forward 93.0%**、decode 6.7%（与频率环境无关）。
暖机参考值（ABBA 对照中热透的 serial 实例，§6）：**p50 32.9 ms / p95 35.7 ms**
（全类混合；冷启动可使 p95 虚高 46%+）。

要点：成本几乎全部在 forward，且随宽度近似线性（87 px → 320 px 约 1:3.4）。
基线覆盖率 100%（真实模型完整读出渲染语料），20 次重复文本/置信度逐位一致。
对"摊销 forward"类优化（packing/batching）而言，可摊销上限就是这 93%；对
"减少每行宽度"类优化而言，medium→short 的成本梯度就是上限。

## 4. 宽度策略臂（对照 serial）

| 臂 | p50 | p95 | wall（96 行 × 20） | 文本一致率 | 置信度最大差 |
| --- | --- | --- | --- | --- | --- |
| serial（动态宽 ≤320） | 47.9 ms | 150.7 ms | 111.4 s | 1.0000（基线） | — |
| pad320（固定 320） | 66.1 ms | 66.8 ms | 117.4 s | 0.8229 | 0.6188 |
| bucket32（32 px 分桶） | 35.4 ms | 66.3 ms | 83.4 s | 0.9688 | 0.1773 |
| bucket64（64 px 分桶） | 65.6 ms | 66.5 ms | 98.2 s | 0.9062 | 0.6258 |

**性能列不可归因**：各臂在主矩阵中依次后跑，powersave 频率爬坡与臂顺序混叠
（§6 ABBA 对照证实同类伪影可使 p95 摆动 ±46%）。**质量两列（文本一致率、置信度
差）与执行时机无关，是本节可靠的结论来源。**

- **pad320**：文本一致率 82.3%——固定宽直接否决，动态宽度策略保持。
- **bucket32**：96 行中 3 行文本改变、置信度最大漂移 0.177。主矩阵中 p50 −26% 的
  "收益"同样受顺序效应污染（bucket32 在 serial 之后运行）；即使收益为真，
  AC8-09 要求"文本一致 + 置信度差 ≤1e-4"，而 padding 改变 CTC 时间步序列，输出
  差异无法压到门槛内——**机制上不可修复**，no-go 是结论性的。
- **bucket64**：一致率更低（90.6%），同样否决。

## 5. packing 与 batching

| 臂 | 单元 | p50 | wall | 组级文本匹配 | 首行前缀匹配 | VM HWM |
| --- | --- | --- | --- | --- | --- | --- |
| pack4cap（4 行拼接，仍钳 320） | 24 组 | 66.1 ms | 31.2 s | 0.083 | 0.333 | 104 MB |
| pack4wide（拼接，钳 4096） | 24 组 | 241.3 ms | 191.4 s | 0.208 | 0.750 | **509 MB** |

（wall 按"同 1920 行内容"折算：serial 111.4 s 处理 96 行 × 20 轮；pack 臂把同样内容
作为 24 组 × 20 轮处理。）

- **pack4cap**：拼接后被 320 上限压缩，模型把 4 行读成乱码，组级只有 8.3% 完整
  匹配。其 wall 31.2 s 看似"快 3.6 倍"，实际是单个 320 宽 forward 只承载了 1/4 的
  像素量——省掉了工作而不是更高效，质量崩塌即代价。契约内 packing 不可用。
- **pack4wide**：解除上限后质量仍只有 20.8%（rec 的池化/CTC 结构把多行特征混叠；
  首行前缀匹配 75% 只说明第一行尚可辨认），同样的 1920 行内容 wall 191.4 s
  （serial 的 1.72 倍——且这是 pack4wide 在频率已爬坡的有利时机下测得的保守下界），
  峰值 RSS 509 MB（serial 的 4.9 倍，与频率无关）。质量、性能、内存三输，no-go。
- **batching（无臂，能力性 no-go）**：pinned rec 计算图首层为
  `Input x 0 1 x`（3 维，无样本维，权重 bundle `rec_param` 第 3 行）；pinned ncnn
  `Extractor::input(blob, Mat)` 每输入一个 Mat，`ncnn::Mat` 维度上限 4 且最高维是
  通道（`src/mat.h` `Mat::shape()`：dims ∈ {1,2,3,4} = w / w×h / w×h×c / w×h×d×c，
  无 batch/n 轴）；多输入的 `run_multi` 是多 blob 而非多样本（DEC-025 §1 已声明）。
  真 batching 需要带 batch 轴的模型重导出与逐层 batch 语义审查，超出"不改 pinned
  runtime/权重"的边界（M8 计划范围约束）。按 AC8-09"伪 batch 不计"，不做伪装测量。

## 6. 线程扩展 ABBA 对照与 dense 端到端对照

主矩阵中 threads2/threads4 呈现大幅 p95 优势（−59%/−70%），且 8 臂 × 20 reps 全部
逐位确定（含 threads>1，NOTE 计数 0）。为排除 powersave 频率爬坡的顺序效应，追加
同进程 ABBA 对照（serial → threads4 → serial → threads4，8 reps × 96 行）：

| 实例（按运行顺序） | p50 | p95 | 与同位对手比 |
| --- | --- | --- | --- |
| serial #1（冷启动） | 34.2 ms | 66.5 ms | — |
| threads4 #1 | 39.8 ms | 44.9 ms | p95 快 32.5%（对冷 serial） |
| serial #2（暖机） | **32.9 ms** | **35.7 ms** | — |
| threads4 #2（暖机） | 37.8 ms | 44.0 ms | **p50 慢 12.8%、p95 慢 23.5%（对暖 serial）** |

- 暖机后（最干净的同位对比）**serial 单线程全面优于 threads4**；主矩阵的"多线程
  加速"是 serial 先跑、撞上低频态的顺序伪影。机制足迹：serial #1 的 66 ms 长尾
  集中在先运行的 short/medium/overlong 类，最后运行的 long 类干净（p95 35.9 ms）；
  serial #2 四类全部干净（p95 35.0–37.3 ms）；threads4 因 4 核满载使 governor 快速
  升频，两实例分布稳定（p95 42.9–46.6 ms）。
- serial 自身漂移：p50 −3.8%，p95 −46.4%（66.5 → 35.7 ms）；threads4 自身 p95 漂移
  仅 −1.9%。**结论：本机上 ncnn 单 forward 多线程不减少总浮点工作量，线程协调
  开销使单元延迟净增；维持 `num_threads=1` 默认（与 DEC-012 位确定性契约一致），
  DEC-025 备选方案"线程/绑核已报告负优化"在本机受控条件下得到复现。**
- 附带发现：本机混合架构上 threads2/threads4 的输出与 threads=1 逐位一致
  （8 臂 × 20 reps + ABBA 全程零差异）。这是本机经验观察，不改变契约对
  threads=1 的要求（跨平台位确定性无法由本机观察保证）。

dense 端到端对照（1600×1000、338 文本块、det max_boxes=1024、threads=1、5 reps）：

- `detected = recognized = 338 = declared_lines`，`deferred = 0`，
  `stop = kCompleted`，5 次报告逐位一致（`DENSE determinism=identical`）。
- wall 4.40–7.52 s；det forward 仅占 wall 的 2–6%，rec 前向合计 4.0–6.7 s
  （338 个短行 × 约 12–20 ms/行，与单元语料的 short 类量级一致）。
- 观察记录（非达标声明）：本机 powersave 下 338 短行的全识别 wall 已在 5 s 门槛
  附近波动；该语料行宽偏短、不代表真实桌面混合宽度密度，5 秒判定以 M8-06 在
  冻结参考环境的协议数据为准。

## 7. 采用决定与后续

- **无候选达标 AC8-09 采纳门槛；全部记录 no-go，M8 不因本实验修改任何默认值**，
  有界逐行实现（M8-03）保持不变。no-go 不阻塞 M8-06（计划既定）。
- 交给 M8-06 的实测输入：`rec_unit_upper_bound` 推导参考——本机暖机 serial
  p95 35.7 ms/行（短中长混合 96 行语料），冻结时按参考机重测；dense 对照展示了
  会话/后端计数与阶段分解在真实模型上的端到端一致性（338/338、kCompleted、
  determinism=identical），可作为 M8-06 命令的冒烟预期。
- **对 M8-06 基准协议的建议**：真实模型性能测量必须控制频率环境——同进程 ABBA
  （或固定 performance governor + 预热）后再报相对比较；单顺序轮次的臂间差在此
  机器族上不可归因。本报告 §4 的性能列即为反例证据。
- 后续可选杠杆（超出 M8 范围，不做承诺）：`use_packing_layout` 对 rec 吞吐的影响
  （本实验在 M5 张量面契约的 unpacked 路径上测量；packed 内核更快但会改变张量面
  契约，需独立决策）；bucketing 若未来模型/解码支持"pad 不变式"（CTC 对 pad 稳定）
  可重启评估。

## 8. 局限

- governor 为 powersave：同进程内臂顺序与频率爬坡混叠，主矩阵的性能列（§3/§4/
  §5 wall）不可直接归因；**质量（文本/置信度）、确定性、内存与覆盖率结论不受
  影响**。线程与 bucket32 的性能对比已用 ABBA 顺序对照修正（§6），其余臂的相对
  性能未做 ABBA 复跑，本报告不据此下性能结论。
- 语料为渲染文本（非真实桌面截图）；行宽分布偏均匀，未覆盖倾斜/低对比度等
  det 质量谱——本实验只测 rec 单元，det 谱外问题不影响结论方向；dense 帧全部
  为短行格子，其 wall 不能外推到混合宽度真实桌面密度。
- 本机为混合架构消费级 CPU；AC8-01/02/05 的 5 秒门槛判定仍以 M8-06 的冻结参考机
  数据为准，本报告数字不用于声明性能达标。
- dense 对照使用 `postprocess.max_boxes=1024` 的实验配置（工具内显式设置），不是
  对默认 256 预算的重新评审。
- 实验过程中发现并修复了工具自身一处缺陷（dense 臂未显式设置 det box 预算，
  默认 256 < 338 框导致 `kBudgetExceeded`）；修复仅涉及工具文件，库代码零改动，
  修复后 dense 复跑 exit 0。主矩阵的 rec 臂数据在该修复之前产生，但修复不影响
  rec 路径，矩阵数据仍然有效。

## 9. 证据清单

- 主矩阵（8 臂 × 20 reps）：`/tmp/m8_iva_results/{stdout.txt, summary.txt,
  units.csv, stderr.txt, exit_code.txt}`；dense 复跑：`stdout_dense_fix.txt`；
  ABBA 对照：`stdout_abba.txt, abba_units.csv, abba_summary.txt`（全部为
  Independent-Verification-Agent 会话产物，复核命令见 §2 与 §6 设置）。
- 语料复现：`diff -r` 两次生成逐字节一致；生成参数与 face 见 §2。
- 交叉验证：IVA 从 units.csv 重算 serial/threads4 的 p50/p95 与 summary 逐位一致；
  CSV 行数 12481 = 6×1920 + 2×480 + 1 表头；ABBA CSV 3073 行 = 4×768 + 表头，
  `uniq -c` 确认严格分块。
- 工具门槛：四次运行（主矩阵、serial 复跑、dense 复跑、ABBA）exit 0 或按预期
  gate 行为通过；GATE `determinism=pass baseline_text_coverage=1.0000` 全部一致；
  8 臂 × 20 reps 输出逐位一致（含 threads2/4，NOTE 计数 0）。
- 执行者：Independent-Verification-Agent（构建、语料、计量、交叉验证与补充
  对照均由其独立完成；主循环仅实现工具与分析数字）。
