# DEC-019：跨帧目标跟踪能力立项（低负载 SOT 与级联重检测）

> 状态：Accepted（2026-09-21，负责人在 M7 启动指示中批准："依照设计与计划，
> 继续下一阶段开发"；目标池/状态机/阈值契约最迟 M7-01 冻结，门槛初值最迟
> M7-09 校准冻结）
> 日期：2026-09-21
> 负责人：linductor
> 替代/被替代：无（推广 [DEC-010](DEC-010-stable-id-matching.md) 第 4 节预留
> 的跨帧演进通道，不替代该记录）
> 修订：2026-09-21 按负责人指示改写决策第 6 条——轻量深度 tracker
> （`TrackerBackend` SPI + NanoTrack 参考后端）由"触发后演进"升级为计划性
> 交付，新增关联 [DEC-020](DEC-020-tracker-backend-spi.md) 与 `RISK-2026-18`
> 关联：[跟踪设计](../design/object-tracking-design.md)、
> [M7 里程碑](../plans/m7-cross-frame-object-tracking.md)、
> [DEC-017](DEC-017-geometric-region-proposal-experiment.md)、
> [DEC-018](DEC-018-geometric-region-proposal-promotion.md)

## 背景与问题

上层系统（Mira Agent 等）需要跨帧稳定指认同一视觉对象：动作校验要确认"点过
的按钮还是那个按钮"，工作流要跟踪滚出又滚入视野的控件。现有能力只做相邻
快照的稳定 ID 关联（`DEC-010`）：画面局部变化、滚动或短暂遮挡即断 ID，
generation 递增后上层被迫重新感知，重复触发高负载 OCR/Detector——与"画面
未显著变化时近零成本复用"的产品目标相悖。

2026-09 调研结论（详见[跟踪设计](../design/object-tracking-design.md)第 1 节）：

1. 低负载持续跟踪在终端/UI 场景可行：目标刚性、静止占空比高、位移温和；
   经典轻量 tracker（MOSSE/KCF/CSRT）与轻量深度 tracker（Ocean/LightTrack/
   NanoTrack）给出各档成本参照，但深度方案在本场景成本高 2–3 个数量级而
   收益有限。
2. "轻量持续跟踪 + 丢失后高负载重检测"是长时跟踪领域标准范式（TLD 一脉，
   VOT-LT 共识），置信度判定（PSR/APCE）与两段式重捕获（粗召回 + 外观复核）
   均有成熟公开方法。
3. 位置-时间先验作为身份证据统计上有条件成立：静止期似然比高；滚动期未经
   补偿会结构性反转；布局突变期整体失效——必须配运动补偿与代际隔离，且
   只能作门控与占位证据，身份确认仍需外观或语义证据。

需要决策的缺口：跟踪能力的架构落点、证据模型、丢失语义、验证与转正路径，
以及与 `DEC-010`/`DEC-018` 两条既有决策线的边界。

## 决策

1. **新增 `SCOPE-13`（跨帧目标跟踪）与 M7 里程碑**。能力落点 `mirador::fusion`
   （状态机、目标池、证据融合、重检测策略原语，随 `PerceptionSession` 持有，
   `DEC-013` 依赖方向不变）；`mirador::image` 新增全局位移估计原语（纯 CPU、
   确定性、预算内）；模板 NCC 复用 M3 组件；闭合结构证据经已冻结的
   `GeometricRegionProposal` 契约（`DEC-018` 阶段 1）消费，不依赖阶段 2。
   阶段 A 零新依赖，不触及核心链接闭包（架构测试口径不变）。
2. **双通道证据模型**：外观通道（模板 NCC 峰值+峰旁瓣质量、闭合结构一致性）
   与位置-时间通道（运动补偿后门控、按运动状态条件化权重）+ 语义兼容谓词。
   位置先验只作门控与占位；确认延续需外观或语义证据。impostor 负模板机制
   对抗 similar-icons swap。规则确定性、可配置、可解释，同 `DEC-010` 风格。
3. **丢失语义显式化**：`kTracking/kUncertain/kLost/kTerminated` 四态状态机；
   `kUncertain` 输出占位框并标记低置信（generation 语义允许上层拒绝）；
   `kLost` 记录中断；`kTerminated` 失败可见。重检测为上层按需 Detector 调用
   （`RULE-12`）：Mirador 提供退避/预算/变化门控联动原语与候选身份复核
   （池模板 + E2 通道），不硬编码触发策略。
4. **验证与转正沿用"证据强度与承诺深度对齐"模式**（`DEC-017`/`DEC-018`
   先例）：合成 harness 先行，指标含 ID 延续正确率（静止/补偿后滚动分开）、
   swap 率、假阳性延续率、丢失误判率（双向）、重捕获成功率/延迟、验证路径
   p50/p95、Detector 触发频率、目标池内存；方法对比矩阵 A/B/C/D 隔离证据
   通道贡献。`ObjectTracker` 契约在 M7 内 Experimental（同
   `geometric_proposal` 模式），不进入兼容性承诺；M7-09 发布基准，M7-10
   go/no-go 判定，GO 后另立决策冻结契约。真实截图评估为转正前置，与
   `DEC-018` 阶段 2 共享 `~/mirador-eval/` 数据采集（一次采集两用）。
5. **晋升门槛初值（暂定，M7-09 校准前不作为验收依据）**：`*-static-page`
   ID 延续正确率 ≥ 0.95；`*-scroll`（补偿后）≥ 0.90；`*-similar-icons`
   swap 率 ≤ 0.05；假阳性延续率 ≤ 0.02；静止帧跟踪短路路径不引入可测回归
   （对照 M1 变化检测基线）；`kLost` 后静止画面零 Detector 触发。任一初值
   在 M7-09 校准后变更需记录理由。
6. **轻量深度 tracker 计划性引入为可选增强通道；CF tracker 维持延后**。
   `POST-06` 升级为计划性交付物：新增 `TrackerBackend` SPI（有状态 backend
   的 init/update 生命周期为既有 SPI 首次引入的语义扩展，契约见
   [DEC-020](DEC-020-tracker-backend-spi.md)）与 NanoTrack ncnn 参考后端
   （`integrations/`，复用 `DEC-015` 已冻结的 `NcnnRuntime` 与
   `MIRADOR_BUILD_INTEGRATIONS` 默认 OFF 机制；权重不入仓、用户显式路径
   提供，引入前完成许可证与模型来源审查并登记 `docs/supply-chain/`）。
   定位：传统双通道为主路径，深度通道为形变/遮挡密集输入与快速位移的
   可选增强，未注入时管线零变化；不进核心发布包、不随核心版本承诺。
   随 M7-11~13 交付。`POST-07`（CF tracker 进 geometry 可选实现）维持
   延后：触发条件不变。

## 备选方案

- **跟踪状态内置 `mirador::image`**：跟踪本质是跨帧身份语义而非像素处理，
  且需访问融合区域与稳定 ID；违反 `DEC-013` 会话归属与依赖方向，被否。
- **深度 tracker 作为起步主路径**：UI 常规场景下成本高 2–3 个数量级而收益
  有限（目标刚性使传统证据通道已够用），且主路径即依赖 runtime 违反"证据
  强度与承诺深度对齐"，被否——采用"传统通道为主路径、深度通道为可选增强"
  的分层定位（2026-09-21 按负责人指示将深度通道由"触发后演进"升级为
  计划性引入，但仍不进核心、不随核心版本承诺，证据门槛不豁免）。
- **不做持续跟踪，丢失即全屏 YOLO 重检测**：成本不可持续（每分钟高负载
  调用无界），且丢失判定本身需要跟踪状态支撑；与低负载定位相悖，被否。
- **位置先验作为独立身份判据**：滚动期结构性反转、swap 无法自证（调研第
  二部分统计条件化分析），只能作门控与占位；采纳为条件化软证据而非判据。
- **立即冻结 `ObjectTracker` 契约**：仅有间接调研证据、无平台实测，扩大
  兼容性承诺与证据强度不称，被否（采 Experimental → go/no-go → 冻结）。

## 影响与风险

- `RISK-2026-17`：全局运动补偿不足或全局/局部变化误判时，位置先验在滚动/
  布局突变期系统性失效——由 M7-04/M7-07 与 A/B/C/D 对比矩阵中的 B/C 差值
  门控，不达标则收紧代际判定或降级位置通道权重。
- `RISK-2026-16`：similar-icons 场景 swap（impostor 负证据不足）——由负模板
  机制与 swap 率门槛门控；不达标的回退为"相似外观候选一律降级 `kUncertain`"。
- `RISK-2026-18`：深度增强通道与传统双通道置信冲突，或深度 tracker 漂移期
  置信度虚高污染模板池——由保守侧处置（冲突降级 `kUncertain`）与"高置信
  模板更新仅取双通道一致帧"门控（`DEC-020`、M7-13）；深度通道为可选注入，
  不达标注射层可关闭，主路径不受影响。
- 与 `DEC-010` 的边界：本决策不改变静态融合门控（IoU/包含/兼容谓词），
  仅在跟踪会话内为已确认 track 增加时间一致性门控；若 M5 基准口径下的
  `RISK-2026-11`（贪心→匈牙利）被触发，与本决策正交演进。
- 与 `DEC-018` 阶段 2 的边界：M7 只消费已冻结契约的每帧描述量，不进入
  §8 输出模型与 §16 融合证据源；两者真实数据评估共享采集、结论互不阻塞。
- 资源与隐私：目标池全部有界（`RULE-06`），模板为内存灰度 patch，不落盘、
  不联网、不入日志（`RULE-10`）；跟踪为会话内单线程语义，无新增并发面
  （`RULE-03`）。

## 验证方式

- M7-01：契约（`TrackState`/`TargetTrack`/`ObjectTracker`、预算默认值、
  阈值初值、`uncertain_frame_limit` 等）冻结为 Experimental + 伪实现测试。
- M7-11~13：`TrackerBackend` SPI 契约（`DEC-020`）冻结 + Fake TrackerBackend
  编排测试；NanoTrack 参考后端合成模型冒烟、供应链与许可证登记、
  `integrations` CI job；深度通道组合规则与未注入退化路径测试。
- M7-09：合成基准数字发布于 `docs/benchmarks/`（`DEC-011` 口径），A/B/C/D
  对比矩阵 + 门槛初值逐项报告。
- M7-10：go/no-go 判定记录于 M7 里程碑文档；GO 另立决策冻结契约，NO-GO
  记录结论并关闭或调整后重跑（`DEC-017` 模式）。
- 全程：6 预设 + sanitizer + lint 双口径 + 架构测试（core 链接闭包不变）+
  坐标 `DOD-03` 矩阵 + `DOD-04` 缓存失效负向（模板版本/参数变化使验证
  结果失效）。

## 关联文档和工作项

- [跟踪设计](../design/object-tracking-design.md)（详细设计与统计模型）
- [DEC-020](DEC-020-tracker-backend-spi.md)（`TrackerBackend` SPI 契约）
- [M7 里程碑](../plans/m7-cross-frame-object-tracking.md)（工作项与退出条件）
- [DEC-010](DEC-010-stable-id-matching.md)（稳定 ID 门控与演进通道）、
  [DEC-013](DEC-013-perception-session-in-fusion.md)（会话归属）、
  [DEC-015](DEC-015-reference-runtime-selection.md)（ncnn 基础设施复用）、
  [DEC-017](DEC-017-geometric-region-proposal-experiment.md) /
  [DEC-018](DEC-018-geometric-region-proposal-promotion.md)（实验-转正模式
  与阶段边界）
- 总计划 `SCOPE-13`、`POST-06`（已升级计划性交付）、`POST-07`、
  `RISK-2026-17`、`RISK-2026-16`、`RISK-2026-18`
- [evaluation-scenes](../benchmarks/evaluation-scenes.md)（场景复用与离线
  数据接入约定）
