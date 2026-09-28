# DEC-022：跨帧目标跟踪契约冻结（ObjectTracker Experimental 面转正评审草案）

> 状态：Proposed（2026-09-28，M7-10 go/no-go 判定 **GO（合成口径）** 后依
> [DEC-019](DEC-019-cross-frame-object-tracking.md) 第 4 条立档的转正决策
> 草案，**待负责人评审**；批准前 M7 Experimental 面保持 Experimental 标记
> 与"不计兼容性承诺"登记不变）
> 日期：2026-09-28
> 负责人：linductor
> 替代/被替代：无（执行 [DEC-019](DEC-019-cross-frame-object-tracking.md)
> 第 4 条预留的"GO 后另立决策冻结契约"通道，不替代该记录；两阶段转正模式
> 沿用 [DEC-017](DEC-017-geometric-region-proposal-experiment.md)/
> [DEC-018](DEC-018-geometric-region-proposal-promotion.md) 先例）
> 关联：[M7 里程碑 Go/No-Go 判定记录](../plans/m7-cross-frame-object-tracking.md)、
> [跟踪设计](../design/object-tracking-design.md)、
> [M7-09 基准报告](../benchmarks/linux-x64-object-tracking-2026-09.md)

## 背景与问题

M7 跨帧目标跟踪（`DEC-019`）已完成 `M7-01`~`M7-09` 全部交付：公共契约
`object_tracker.hpp`（Experimental）与 `shift_estimation.hpp`、
`stable_id_tracker.hpp` 直通扩展随 M7-01~08 冻结语义于同一 Experimental
面，`M7-09` 发布合成基准（A/B/C/D 方法矩阵 × 六场景 × 设计 §8 八项指标，
`DEC-011` 口径）并完成门槛初值逐项校准。`M7-10` go/no-go 判定（M7 里程碑
"Go/No-Go 判定记录"节）结论为**合成口径 GO**：`DEC-019` 第 5 条六项门槛
全部达标。按 `DEC-019` 第 4 条，GO 后另立决策冻结 `ObjectTracker` 契约；
本文即该决策草案。

需要决策的缺口与 `DEC-018` 立项时同构：现有证据**全部来自合成场景**
（`RISK-2026-14`；真实截图评估尚未执行、与 `DEC-018` 阶段 2 共享
`~/mirador-eval/` 采集、数据不入仓），契约冻结到什么深度才与证据强度相称；
以及三项校准遗留（补偿置信度库默认、峰旁瓣比真实裕度复核、相似外观回退
是否入库默认）由谁在何时裁决。

## 证据基线（M7-10 判定引用）

- `M7-09`（[linux-x64-object-tracking-2026-09](../benchmarks/linux-x64-object-tracking-2026-09.md)，
  `DEC-011` 口径：单机 Linux x64 release、3 次重复非计时指标逐位一致）：
  `DEC-019` 第 5 条六项门槛全部达标——static-page 延续 1.000 ≥ 0.95、
  scroll 补偿后 1.000 ≥ 0.90、similar-icons swap 仅 D 达标（D = 0 ≤ 0.05，
  A/B/C = 0.5）、假阳性延续仅 D 严格通过（D = 0.000 ≤ 0.02）、静止帧短路
  相对 M1 同日基线无可测回归、`kLost` 后静止画面零 Detector 触发（24 cell
  内建断言）。"仅 D 通过"两项的判定口径论证与 A/B/C 差值归因（通道贡献
  隔离的预期结果，`RISK-2026-16`/`RISK-2026-17` 门控证据）见 M7-10 判定
  记录；门槛初值无一处变更。
- 门槛初值逐项校准（`DEC-019` 第 5 条）：库默认全部以测量依据维持；
  `min_compensation_confidence` 库默认 0.0 维持、调用方配置 0.7 为已发布
  参考策略（测量依据与不利面见 M7-10 判定记录，开放项 1）。
- 契约质量证据：M7-01~08 各工作项经 Independent-Verification-Agent 独立
  验证套件 + 六预设 + sanitizer + lint 双口径 + CI 14 job 门禁（PR #20~#29
  全绿，记录于 M7 验证记录）；确定性（同输入逐位）、字节预算（池全程峰值
  39,960 B = 1 MiB 预算的 3.8%，`RULE-06` 显式淘汰/显式错误）、取消/超时
  显式转化、坐标 `DOD-03` 矩阵、隐私 `DOD-06` 负向均有套件钉住。
- 口径限定（随本决策一并生效，不得拆开引用）：**仅有合成证据**
  （`DOD-05`/`RISK-2026-14`）；单机 Linux x64 release（`DEC-011` 补跑
  条件：物理 Android 设备与 Windows 桌面缺失）；swap 率量纲为"每对象交换
  次数"（分母随场景定义冻结）。

## 决策（草案，待评审）

**两阶段转正**，证据强度与承诺深度对齐（模式同 `DEC-018`）：

1. **阶段 1（经负责人批准后生效）：M7 跟踪 Experimental 面契约冻结。**
   撤销三处 Experimental 标记与"不计兼容性承诺"登记，转为正式契约：
   - `include/mirador/object_tracker.hpp`（`TrackState`、`EvidenceGrade`、
     `TrackObservation`/`TrackTemplate`/`TrackSemantics`、`TargetTrack`、
     `ObjectTrackerOptions` 全部冻结默认值（含 M7-09 校准后的库默认）、
     `TrackAdoption`、`ObjectTracker` 池管理及 M7-03~08 帧级管线方法与
     全部配套类型）；
   - `include/mirador/shift_estimation.hpp`（`ShiftEstimationParams`、
     `ShiftEstimate`、`estimate_global_shift` 双入口；校准默认
     thumbnail 64 / max_shift 16 / 512 KiB）；
   - `include/mirador/stable_id_tracker.hpp` 的 M7-06 扩展
     （`ConfirmedAssociation` 与 `advance` 的 `confirmed_associations`
     参数；`DEC-010` M4 冻结静态语义不受影响——空关联列表下既有匹配
     行为逐位不变）。
   同步动作：头文件 Experimental 注记移除、`docs/api/README.md` Experimental
   注记转正式条目、`docs/compatibility/compatibility.md` Experimental 登记
   转正式登记、`DEC-019` 反向注记收口、CHANGELOG（随 `v0.4.0` 收尾）。
   冻结后任何字段/签名/默认值变更走兼容性变更流程。
   - 依据：契约质量证据完整（确定性、预算、坐标矩阵、取消/错误语义、
     sanitizer、lint、CI）；合成门槛全数达标；冻结不扩大核心架构边界——
     全部实现位于 `mirador::fusion`/`mirador::image` 既有闭包内，零新依赖
     （架构测试与链接闭包探针不变）；不改变 `DEC-010` 静态融合语义。
2. **阶段 2（独立立项，不在本决策内执行）：真实截图评估与转正收口。**
   按 [evaluation-scenes](../benchmarks/evaluation-scenes.md) 离线接入约定
   完成真实截图评估并发布数字（`DEC-011` 口径；与 `DEC-018` 阶段 2 共享
   `~/mirador-eval/` 采集，一次采集两用，结论互不阻塞——`DEC-019` 既定
   边界），对照下方"开放项"逐项复核后收口转正结论。
3. **证据登记（不可撤销项）**：本决策草案立档时点，该能力**只有合成证据**
   （`DOD-05`）。阶段 1 的兼容性承诺建立在"API 行为正确且稳定"之上，不
   代表真实场景价值已证明。若阶段 2 真实数据评估否定假设价值，阶段 1 冻结
   的契约**不回滚**（API 已验证、可裁剪、零核心侵入——同 `DEC-018` 第 3 条
   语义）；结论在 M7 里程碑与本文留档。

## 开放项（交负责人裁决，不阻塞本决策评审）

按 M7-10 判定记录转记/新立，均挂真实数据评估复核条件：

1. **`min_compensation_confidence` 库默认是否上调 0.7**：库默认 0.0 维持的
   测量依据（判定范围、置信度度量无真实数据先验、调用方旋钮契约定位）与
   不利面（0.0 门下 C/D 曾对局部变化帧平移整池的实测暴露）见 M7-10 判定
   记录；参考调用方策略 0.7 已发布并被校准套件钉住
   （`HarnessCallerCompensationGate07SeparatesMeasuredBands`）。真实数据
   评估时以真实内容置信度分布复核；负责人亦可裁决在真实数据评估前以集成
   文档约定"调用方必配 0.7"。
2. **`peak_sidelobe_ratio_min` 5.0 真实数据裕度复核**（M7-09 校准表转记）：
   贫纹理合成 patch 实测真匹配 PSR 4.96 < 5.0 被拒（归因于刺激而非阈值），
   场景纹理富化后真匹配 ≥ 6.95、杂峰 ≤ 3.5；真实数据评估必须优先复核该
   裕度。
3. **`RISK-2026-16` 回退是否写入库默认**（相似外观候选一律降级
   `kUncertain` 等）：当前 D 配置实测零 swap、零假阳性延续，回退**不启用**、
   维持调用方可配置原语（`RULE-12` 策略决策权留上层）；是否将回退强制为
   库默认交负责人裁决。

## 备选方案

- **维持 Experimental 不冻结**：六项门槛达标且机制分析完整（通道贡献隔离、
  内建负向断言、预算余量 3.8%），已验证能力长期游离于兼容性纪律之外——
  同 `DEC-018` 备选先例被否（草案立场，待评审确认）。
- **立即全量转正（含真实场景承诺）**：`RISK-2026-14` 未消除即宣称真实
  场景效果，违反 `DOD-05` 与"证据强度与承诺深度对齐"，被否。
- **随本决策一并上调 `min_compensation_confidence` 库默认**：见开放项 1——
  测量依据仅覆盖合成域（`DOD-05`），上调属代码变更（默认值 + 测试校准 +
  全套门禁）且应在真实数据评估的指定复核点一并裁决，本草案不捆绑。

## 影响与风险

- 阶段 1 生效后，三处契约面的任何字段/签名/默认值变更都走兼容性变更流程
  （deprecation 周期），不再享有实验期自由调整权；`ObjectTrackerOptions`
  冻结默认值（含全部 M7-09 校准值）成为承诺面。
- 本决策为 Proposed：负责人批准前，M7 Experimental 登记持续有效，任何
  文档不得引用"已冻结/已计入兼容性承诺"口径。
- `DEC-020`（`TrackerBackend` SPI）按其自身时间表最迟 M7-11 契约冻结，
  与本决策正交：深度增强通道为可选注入（M7-11~13），不阻塞亦不被本决策
  阻塞；GO 判定不改变其计划性交付路径。

## 验证方式

- 阶段 1：批准后一次文档同步（API 索引、兼容性登记、头文件 Experimental
  注记、`DEC-019` 反向注记收口、CHANGELOG、设计文档 §24 M7 状态）+ 全量
  CI 门禁；无代码变更（同 `DEC-018` 阶段 1 先例）。
- 阶段 2：独立立项时按工程规范建立工作项、测试矩阵与退出条件；真实数据
  评估数字按 `DEC-011` 发布于 `docs/benchmarks/`。

## 关联文档和工作项

- [M7 里程碑](../plans/m7-cross-frame-object-tracking.md)（Go/No-Go 判定
  记录——本决策唯一判定依据）
- [DEC-019](DEC-019-cross-frame-object-tracking.md)（判定依据与转正通道）、
  [DEC-018](DEC-018-geometric-region-proposal-promotion.md)/
  [DEC-017](DEC-017-geometric-region-proposal-experiment.md)（两阶段转正
  模式先例）、[DEC-010](DEC-010-stable-id-matching.md)（静态融合语义不变）、
  [DEC-020](DEC-020-tracker-backend-spi.md)（深度通道正交）、
  [DEC-011](DEC-011-benchmark-environments.md)（基准口径限定）
- [M7-09 基准报告](../benchmarks/linux-x64-object-tracking-2026-09.md)
  （证据基线）、[跟踪设计](../design/object-tracking-design.md) §8/§10
- 总计划 `SCOPE-13`、`RISK-2026-14`/`RISK-2026-16`/`RISK-2026-17`
