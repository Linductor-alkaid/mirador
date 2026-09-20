# DEC-017：几何区域 Proposal 实验轨道与契约边界

> 状态：Accepted（2026-09-16，M6-01 内冻结）
> 日期：2026-09-16
> 负责人：linductor
> 冻结里程碑：M6
> 替代/被替代：无（第 3 条的转正通道由
> [DEC-018](DEC-018-geometric-region-proposal-promotion.md) 草案承接，本记录
> 对实验轨道的界定持续有效）
> 关联：[issue #11](https://github.com/Linductor-alkaid/mirador/issues/11)

## 背景与问题

issue #11 提出：闭合/近闭合线段结构可能构成语义区域的有效低成本先验，值得组织为
`GeometricRegionProposal`，为 Cache、OCR、Detector 与 VLM 提供候选区域。该假设目前
没有任何真实数据支撑相关性，issue 明确要求先实验验证再决定是否转为正式能力。需要
冻结的是实验本身的工程边界：代码放哪、API 以什么稳定性承诺发布、验证按什么口径
判定成败，避免实验代码以"临时"名义绕过既有的分层、预算与验证约束。

## 决策

1. **实验轨道立项为 M6**，范围以设计 §24 M6（实验轨道）为准：先证明假设，再谈
   集成。验证不通过时记录结论并关闭或调整后重跑，不默认续期。
2. **代码落在 `mirador::geometry`**：公共头 `include/mirador/geometric_proposal.hpp`、
   实现入 `src/geometry/`，依赖闭包保持 `mirador::core`（仅标准库）。不新增构建
   开关——`MIRADOR_BUILD_GEOMETRY` 已是既有裁剪面；不新建模块目标。
3. **API 非冻结（Experimental 标记）**：头文件与符号以 "Experimental (M6)" 注记，
   字段与签名在 M6 内可调整；`docs/compatibility/` 登记为 experimental，不计入
   兼容性承诺。转正（进入设计 §8 输出模型、作为融合证据源、承诺兼容性）必须另立
   决策记录并同步设计文档与架构测试。
4. **契约底线沿用全局规则，不因实验放宽**：确定性（同输入位稳定）、显式预算
   （线段数、proposal 数上限，超限 `kBudgetExceeded`；非法参数 `kInvalidArgument`）、
   不修改输入、无内部并发、纯 CPU、失败对调用方可见。坐标测试覆盖 0/90/180/270
   与奇数尺寸（`DOD-03`）。
5. **语义边界**：只产出几何完整性证据（`closure_score`、`rectangularity`、
   `edge_support`），不产生 `Button`/`Icon` 等语义标签；最终语义仍由 OCR、Detector、
   Accessibility 与 Fusion 判定。issue 草图中的 `temporal_stability` 依赖跨帧状态，
   推迟到跨帧工作项（`M6-05`），单帧分析不填充该字段。
6. **验证口径**：合成场景指标先行（`M6-04`，ground truth 由生成过程给出）；真实
   截图按 M5-06 评测集约定离线接入（显式路径、数据不入仓、默认不落盘不联网）。
   晋升门槛初值（合成口径，可随验证数据修订并记录）：语义区域 recall ≥ 0.90、
   proposal precision ≥ 0.50、重复 proposal ≤ 2 个/实体、Tight ROI 相对整帧像素
   缩减 ≥ 60%。缓存增益（`Cache Hit Improvement`）只测量、不作门槛——它依赖
   `M6-05` 集成探针。

## 备选方案

- 直接实现进 fusion/核心输出模型：无验证数据即扩大核心承诺，违背 issue #11 的
  "先验证后转正"定位，被否。
- 独立 `mirador::proposal` 实验模块目标：为未验证能力增加一个构建面与架构测试
  维护成本，而 geometry 已拥有 `LineSegment` 输入类型与可裁剪开关，被否。
- 实验代码只放 `src/` 内部不公开头：验证 harness（`benchmarks/`）与调用方探针
  需要稳定包含路径，且 M3 已有"SPI + 一方实现公开头"的先例
  （`segment_growing_line_detector.hpp`），被否。
- 以 feature flag 隐藏在 fusion 内：把未验证假设耦合进已冻结的融合契约
  （`DEC-010`/`DEC-016`），失败回滚成本高，被否。

## 影响与风险

- `mirador::geometry` 新增编译单元，架构测试的模块闭包断言自动覆盖；experimental
  API 进入公共头目录，`docs/api/README.md` 与兼容性登记需同步并标注实验性。
- `RISK-2026-09`（一方线段检测器真实场景质量）直接约束本实验的上限：若线段输入
  质量不足，闭合结构分析无法补救；M6-04 需分别报告"输入线段质量"与"闭合分析
  增益"两个口径，避免把检测器缺陷误判为假设失败。
- 合成场景的指标门槛只证明"可实现性"，不证明真实场景价值；真实数据评估受
  `DEC-011` 同样的环境限制，结论发布时必须带口径限定。

## 验证方式

- `M6-02`/`M6-03` 单元与属性测试（确定性、预算、坐标矩阵、越界防护）由
  Independent-Verification-Agent 独立编写执行。
- `M6-04` 基准入口在合成场景上产出八项指标中的可测项（recall/precision/
  duplicate/ROI reduction/temporal 稳定性占位），数字发布于 `docs/benchmarks/`
  并附 `DEC-011` 口径限定。
- 转正与否的判定记录在 M6 收尾工作项，无论结论如何都留档。

## 关联文档和工作项

- [设计文档 §24 M6](../design/mirador-development-design.md)
- [实施总计划](../plans/mirador-implementation-plan.md)（1.3 修订、`SCOPE-12`）
- [M6 里程碑文档](../plans/m6-geometric-region-proposal-experiment.md)
- issue #11（假设来源与指标清单）
- `RISK-2026-09`（输入质量约束）
