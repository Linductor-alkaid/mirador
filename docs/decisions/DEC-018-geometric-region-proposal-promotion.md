# DEC-018：几何区域 Proposal 转正（契约冻结先行，融合集成待真实数据）

> 状态：Accepted（2026-09-20，经负责人授权批准；阶段 1 契约冻结即日生效，
> 阶段 2 维持"真实截图评估前置、独立立项"不变）
> 日期：2026-09-20
> 负责人：linductor
> 冻结里程碑：M6
> 替代/被替代：无（执行 [DEC-017](DEC-017-geometric-region-proposal-experiment.md)
> 第 3 条预留的"转正另立决策记录"通道，不替代该记录）
> 关联：[issue #11](https://github.com/Linductor-alkaid/mirador/issues/11)、
> [M6 里程碑 go/no-go 判定记录](../plans/m6-geometric-region-proposal-experiment.md)

## 背景与问题

M6 实验轨道（`DEC-017`）已按约定完成全部工作项。`M6-04` 合成验证数字与
`M6-05`（条件触发）跨帧/缓存增益测量均已达门槛或按口径完成记录，go/no-go
判定（M6-06）结论为**合成口径 GO**。按 `DEC-017` 第 3 条，把实验能力晋升为
正式能力（进入设计 §8 输出模型、作为融合证据源、承诺兼容性）必须另立决策
记录并同步设计文档与架构测试；本文即该决策的草案。

需要决策的缺口是：现有证据全部来自**合成场景**（`RISK-2026-14`，真实截图
评估尚未执行、数据按约定不入仓），转正到什么深度才与证据强度相称。

## 证据基线（M6-06 判定引用）

- `M6-04`（[linux-x64-geometric-proposal-2026-09](../benchmarks/linux-x64-geometric-proposal-2026-09.md)）：
  Mode A（闭合分析层）汇总 recall 1.000 / precision 0.875 / 重复 1 个/实体 /
  tight-ROI 缩减 min 0.638——`DEC-017` 第 6 条四项晋升门槛初值全部 PASS；
  precision 折损全部来自装饰负例（0.571），属假设的预期语义边界。Mode B
  （端到端）除圆角（硬边光栅化碎裂，`RISK-2026-09` 输入质量口径）外与
  Mode A 一致；耗时随线段数超线性但 1024 段（预算上限）p50 ≈ 2.3 ms。
- `M6-05`（[linux-x64-geometric-proposal-reuse-2026-09](../benchmarks/linux-x64-geometric-proposal-reuse-2026-09.md)）：
  ±2 px 独立抖动 6 帧序列关联率 1.000、配对 IoU ≥ 0.979；VisualIndex 对照
  中视觉歧义场景 top-1 0.250 → 几何门控 0.625，视觉可分场景零代价
  （增益只测量、不作门槛，`DEC-017` 第 6 条）。
- 口径限定（全部引用随判定生效）：仅合成证据（`RISK-2026-14`）；
  `DEC-011` 单一 Linux x64 环境；实验实现已过 6 预设 + sanitizer + lint
  双口径与 CI 14 job 门禁。

## 决策

**两阶段转正**，证据强度与承诺深度对齐（2026-09-20 经负责人授权批准）：

1. **阶段 1（批准即生效）：契约冻结。** `include/mirador/geometric_proposal.hpp`
   撤销 "Experimental (M6)" 标记，纳入兼容性承诺：`docs/api/README.md` 从
   experimental 注记转为正式条目，`docs/compatibility/compatibility.md`
   experimental 登记转为正式登记，`DEC-017` 第 3 条对该头的豁免终止。
   公共契约（`GeometricRegionProposal`、`GeometricProposalParams`、
   `propose_regions` 签名与错误语义）自批准起按既有变更纪律管理。
   - 依据：契约质量证据完整（确定性、预算、坐标矩阵、越界防护、sanitizer、
     lint、CI），合成门槛全数 PASS；冻结本身不扩大核心架构边界——API 仍位于
     `mirador::geometry`，链接闭包仍仅标准库，不进入融合与输出模型。
2. **阶段 2（独立立项，不在本决策内执行）：融合与输出模型集成。** 把
   几何 proposal 作为设计 §16 融合证据源、`temporal_stability` 正式契约
   （以 `M6-05` 实验 1 探针为基线）与 VisualIndex 几何门控正式化（实验 2）
   纳入设计 §8 输出模型。**立项前置条件：按
   [evaluation-scenes](../benchmarks/evaluation-scenes.md) 离线接入约定完成
   真实截图评估并发布数字**（`DEC-011` 口径），或负责人明确接受仅合成证据
   并记录风险接受。涉及设计文档 §8/§16/§24 修订、架构测试扩展与新增决策
   记录，按工程规范流程另行评审。
3. **证据登记（不可撤销项）**：本决策批准时点，该能力**只有合成证据**；
   阶段 1 的兼容性承诺建立在"API 行为正确且稳定"之上，不代表真实场景
   相关性已证明。若后续真实数据评估否定了假设价值，阶段 1 冻结的契约
   **不回滚**（API 已验证、可裁剪、零核心侵入），阶段 2 不立项即视为
   假设关闭；结论在 issue #11 与里程碑文档留档。

## 备选方案

- **立即全量转正（含融合集成）**：基于纯合成证据扩大核心承诺
  （fusion 证据源 + 输出模型变更），`RISK-2026-14` 未消除即冻结跨帧契约，
  回滚成本高，被否。
- **不转正，维持 Experimental 并关闭实验**：合成门槛全数 PASS 且机制分析
  完整（装饰负例边界、门控残余 wrong 的几何归因），证据支持至少冻结契约；
  维持"实验"标记反而使已验证能力长期游离于兼容性纪律之外，被否。
- **只转正阶段 2、跳过阶段 1**：未冻结契约先进入融合会让融合依赖一个
  可自由变更的实验面，违反"先契约后实现"的既定拆分原则，被否。

## 影响与风险

- 阶段 1 生效后，`geometric_proposal.hpp` 的任何字段/签名变更都走兼容性
  变更流程（deprecation 周期），不再享有实验期的自由调整权。
- 阶段 2 若在真实数据不达标时被否，`M6-05` 的缓存增益数字仅留档为
  实验测量，不构成任何集成承诺；无已发布功能受影响。
- 两阶段拆分使"转正"状态需要读两个文档才能完整理解——本文与关联决策、
  里程碑 go/no-go 记录互相链接以缓解。

## 验证方式

- 阶段 1：批准后一次文档同步（API 索引、兼容性登记、`DEC-017` 反向注记、
  CHANGELOG、设计 §24 M6 状态）+ 全量 CI 门禁；无代码变更。
- 阶段 2：独立里程碑立项时按工程规范建立工作项、测试矩阵与退出条件；
  真实数据评估数字按 `DEC-011` 发布于 `docs/benchmarks/`。

## 关联文档和工作项

- [DEC-017](DEC-017-geometric-region-proposal-experiment.md)（实验轨道与转正通道）
- [M6 里程碑](../plans/m6-geometric-region-proposal-experiment.md)（go/no-go 判定记录）
- [实施总计划](../plans/mirador-implementation-plan.md)（`SCOPE-12`）
- issue #11（假设来源；实验结论链接随 M6-06 留档）
- [设计文档 §8/§16/§24](../design/mirador-development-design.md)（阶段 2 修订对象）
