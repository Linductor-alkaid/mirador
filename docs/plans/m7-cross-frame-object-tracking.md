# M7：跨帧目标跟踪（低负载 SOT 与级联重检测）

> 状态：In Progress（2026-09-21 立项生效：[DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md)
> / [DEC-020](../decisions/DEC-020-tracker-backend-spi.md) 经负责人批准转 Accepted）
> 负责人：linductor
> 所属计划：[Mirador 实施总计划](mirador-implementation-plan.md)（`SCOPE-13`）
> 前置：M6（已完成）；建议与 `DEC-018` 阶段 2 的真实数据评估协调排期，但不互为前置
> 建议发布点：`v0.4.0`（暂定，随判定收尾确认）
> 更新日期：2026-09-28

## 目标

在 `PerceptionSession` 内对同一视觉对象跨帧维持身份与位置估计：常态路径以
变化检测门控做到近零成本复用，局部变化时以模板 NCC + 闭合结构双通道完成
邻域验证，丢失时以显式状态与预算化原语支持上层触发高负载重检测。交付后
"画面未变/局部变化/目标丢失"三种场景的感知成本结构从"重新感知"变为
"短路复用 + 按需验证 + 按需重检测"。

## 范围与非目标

**范围**：跟踪状态机与目标池（fusion）；变化检测门控三级短路；全局位移
估计原语（image）；邻域验证（模板 NCC 峰值+峰旁瓣质量、闭合结构一致性）；
全局运动补偿与布局代际；证据条件化融合与丢失判定；级联重检测原语与身份
复核；`TrackerBackend` SPI 契约（`DEC-020`）与 NanoTrack ncnn 参考后端
（`integrations/`）及深度增强通道的条件化融合；合成验证 harness 与
A/B/C/D 基准发布；go/no-go 判定。

**非目标**：MOT 全局最优关联；CF tracker（`POST-07`）；重检测触发策略
硬编码（`RULE-12`）；跨会话/跨设备身份唯一（`RULE-09`）；`DEC-018` 阶段 2
的输出模型与融合证据源集成；深度 tracker 权重入仓或进核心发布包。

## 设计与决策依据

- [跟踪设计](../design/object-tracking-design.md)（统计模型、状态机、管线、
  测试矩阵的权威来源）
- [DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md)（能力边界、
  证据模型、转正路径、门槛初值）
- [DEC-010](../decisions/DEC-010-stable-id-matching.md)（稳定 ID 门控；本
  里程碑扩展其跨帧通道，不改变静态融合语义）
- [DEC-020](../decisions/DEC-020-tracker-backend-spi.md)（`TrackerBackend`
  SPI 契约：有状态会话句柄、同步边界、缓存语义豁免界定）
- [DEC-015](../decisions/DEC-015-reference-runtime-selection.md)（ncnn
  基础设施复用：`NcnnRuntime`、`integrations/` 默认零获取、评测接入分层）
- [DEC-017](../decisions/DEC-017-geometric-region-proposal-experiment.md) /
  [DEC-018](../decisions/DEC-018-geometric-region-proposal-promotion.md)
  （Experimental → go/no-go → 冻结的转正模式；阶段 1 契约的每帧描述量消费）
- [evaluation-scenes](../benchmarks/evaluation-scenes.md)（场景复用与离线
  数据接入；真实数据与 `DEC-018` 阶段 2 共享采集）

## 工作项

- [x] `M7-01` 契约冻结：`TrackState`/`TargetTrack`/`ObjectTracker` 公共契约
  （`mirador::fusion`，Experimental 标记）与全部默认值（目标数、位置历史上限、
  模板/负模板数、字节预算、`uncertain_frame_limit`、验证阈值初值、重检测
  退避初值），附伪实现测试与确定性/取消/错误语义说明；同步 API 索引与
  兼容性 Experimental 登记。
- [x] `M7-02` 目标池有界结构：track 生命周期管理、LRU/最旧优先显式淘汰、
  按布局代际分组的位置历史、模板与负模板存储；预算超限为显式淘汰并计入
  trace（`RULE-06` 负向测试：超预算不静默增长、不静默丢弃）。
- [x] `M7-03` 变化检测门控三级短路：画面未变/变化 ROI 不相交的近零路径、
  ROI 相交触发的验证入口；同帧多 track 独立短路；短路路径不引入相对 M1
  变化检测基线的可测回归（基准对照）。
- [x] `M7-04` 全局位移估计原语（`mirador::image`）：低分辨率平移搜索、
  纯 CPU 确定性、预算保护；输出位移向量 + 置信度；坐标链经 `Transform2D`
  组合并通过方向/奇数尺寸/往返容差矩阵（`DOD-03`）。
- [x] `M7-05` 邻域验证器：验证 ROI 内模板 NCC（多模板最优 + 峰旁瓣质量）
  与 `GeometricRegionProposal` 闭合结构一致性（描述量与池内基线偏差容差）
  双通道；模板版本/参数变化使验证结果失效（`DOD-04` 负向）。
- [x] `M7-06` 证据融合与状态机：静止/补偿后滚动/代际切换的条件化权重，
  E1/E2/位置/语义四级证据分级（确认/临时延续/占位/否决，含 impostor 负
  模板排除）；`kTracking/kUncertain/kLost/kTerminated` 转移与 `DEC-010`
  tracker 对接（已确认 track 门控直通）。
- [x] `M7-07` 全局运动补偿与布局代际集成：全局变化分类 → 代际递增 → 位置
  先验条件化（清零/降权）、track 降级与超阈值转 `kLost`；补偿后位置恢复
  验证（滚动场景往返）。
- [x] `M7-08` 级联重检测原语与身份复核：退避序列、最大重试、变化门控联动
  （静止画面零触发负向测试）、预算耗尽显式失败；重检测候选经池模板 + E2
  复核后延续/新分配 ID 并记录中断事件；策略决策权留给上层（`RULE-12`）。
- [x] `M7-09` 合成验证 harness 与基准发布：A/B/C/D 方法对比矩阵、第 3 节
  指标全套数字、场景覆盖 `*-static-page`/`*-scroll`/`*-dialog`/
  `*-theme-switch`/`*-similar-icons`/`*-partial-anim`；数字发布于
  `docs/benchmarks/`（`DEC-011` 口径）；门槛初值逐项校准冻结。
- [x] `M7-10` go/no-go 判定与转正决策草案：依 M7-09 数字对照 `DEC-019`
  门槛逐项判定并记录于本里程碑"验证记录"；GO 另立决策冻结
  `ObjectTracker` 契约并纳入兼容性承诺；NO-GO 记录结论、归因与重跑或关闭
  建议（`DEC-017` 模式）。判定为**GO（合成口径）**（2026-09-28，判定记录
  见下方"Go/No-Go 判定记录"节）；转正草案
  [DEC-022](../decisions/DEC-022-object-tracker-contract-freeze.md)
  （Proposed，待负责人评审）。
- [x] `M7-11` `TrackerBackend` SPI 契约冻结（`DEC-020`）：接口形态、状态
  归属与生命周期、同步/取消语义、缓存豁免界定；Fake `TrackerBackend`
  （固定轨迹注入）完成 fusion 侧编排测试；架构测试证明 core 链接闭包
  不变；API 索引与兼容性 Experimental 登记。
- [x] `M7-12` NanoTrack ncnn 参考后端（`integrations/`）：许可证与模型
  来源审查并登记 `docs/supply-chain/`（审查未通过则按 `DEC-020` 备选更换
  候选，契约不变）；复用 `NcnnRuntime` 与 `MIRADOR_BUILD_INTEGRATIONS`
  默认 OFF；合成模型冒烟入 `integrations` 套件与 CI job；真实权重评测按
  `DEC-015` 分层走用户显式路径（`RISK-2026-13` 口径）。（审查**通过**并
  登记 [docs/supply-chain/nanotrack.md](../supply-chain/nanotrack.md)，
  未触发备选更换；交付与验证轮记录见下方"验证记录"2026-09-28 三条。）
- [ ] `M7-13` 深度增强通道条件化融合：注入时 E1/深度组合与置信冲突保守
  处置（`RISK-2026-18` 门控）、高置信模板更新仅取双通道一致帧、未注入
  退化路径零变化验证、上层启用开关（`RULE-12`）；`A/B/C/D` 基准追加
  "D+深度增强"口径列报（不阻塞 M7-10 传统口径判定）。

## 风险与阻塞

- `RISK-2026-17` 运动补偿不足或全局/局部变化误判导致滚动/布局突变期位置
  先验系统性失效——门控：A/B/C/D 矩阵 B/C 差值与 `*-scroll` 延续率；回退：
  收紧代际判定或位置通道降权。
- `RISK-2026-16` `*-similar-icons` swap（负证据不足）——门控：swap 率门槛；
  回退：相似外观候选一律降级 `kUncertain`。
- 真实截图评估数据未采集（`RISK-2026-14` 既有风险）：不阻塞合成口径判定，
  但阻塞转正决策（与 `DEC-018` 阶段 2 同前置）；与负责人协调共享采集。
- `RISK-2026-18` 深度增强通道与传统双通道置信冲突或深度 tracker 漂移污染
  模板池——门控：冲突保守降级 + 高置信模板更新仅取双通道一致帧（M7-13）；
  深度通道为可选注入，不达标时上层可关闭，主路径不受影响。
- NanoTrack 许可证与模型来源审查未通过会阻塞 `M7-12`——按 `DEC-020` 备选
  更换候选（LightTrack/Ocean ncnn 移植），契约不变，不阻塞 M7 主线。
- 阈值初值无先验：以 M7-09 校准为准，初值仅用于开发冒烟（`DEC-019` 第 5 条）。

## Go/No-Go 判定记录（`M7-10`，2026-09-28）

**结论：GO（合成口径）。** `DEC-019` 第 5 条六项晋升门槛初值依
`M7-09` 发布基准逐项判定全部达标（判定口径与归因见下表；门槛初值无一处
变更，`min_compensation_confidence` 库默认维持 0.0 的裁定见下）。转正路径
按 `DEC-019` 第 4 条另立决策草案
[DEC-022](../decisions/DEC-022-object-tracker-contract-freeze.md)
（Proposed，待负责人评审），**M7 Experimental 面（`object_tracker.hpp`、
`shift_estimation.hpp`、`stable_id_tracker.hpp` 直通扩展）在其批准前保持
Experimental 标记与"不计兼容性承诺"登记不变**（同 M6-06 → `DEC-018`
先例）。

| 门槛（`DEC-019` 第 5 条初值，无变更） | 判定口径 | 测量（`M7-09` 发布） | 判定与归因 |
| --- | --- | --- | --- |
| `*-static-page` ID 延续正确率 ≥ 0.95 | 四方法全部 | 1.000（234/234，三方法短路零验证调用；A 全验证路径亦 1.000） | **通过** |
| `*-scroll`（补偿后）ID 延续正确率 ≥ 0.90 | 补偿后方法（C/D） | C/D = 1.000（138/138 静止期 + 36/36 滚动期）；B（无补偿）静止期 0.333 | **通过**。门槛口径即"补偿后"：B/C 差值（延续 0.333 vs 1.000、Detector 触发 120/min vs 0/min）源于滚动步长 48 px 刻意超出冻结验证 ROI 半径 ±36 px，C 经补偿于 (0,−3) px 恢复强匹配（实测置信度 [0.93, 0.95]）——`RISK-2026-17` 门控证据（证明补偿通道必要性），不构成门槛失败 |
| `*-similar-icons` swap 率 ≤ 0.05 | D（全通道语义口径） | D = 0（0 次交换 / 4 对象）；A/B/C = 0.5（2 次 / 4 对象） | **通过（D 口径）**。归因见下方"口径论证" |
| 假阳性延续率 ≤ 0.02 | D（全通道语义口径） | D = 0/1 确认提交 = 0.000；A = 2/116 ≈ 0.017（名义达标但同方法 swap 不达标）；B/C = 2/2 = 1.0 | **通过（D 口径，严格）**。归因见下方"口径论证" |
| 静止帧跟踪短路相对 M1 变化检测基线无可测回归 | 同机同日 M1 复测对照 | detect 单独 p50 3455.7 µs vs 管线前缀 p50 3448.2 µs；M1 同日 unchanged p50 3501.6 µs | **通过**（差值在运行噪声内，与 M7-03 发布的 gate-only 0.09–0.34 µs 一致） |
| `kLost` 后静止画面零 Detector 触发 | 全部 24 cell（A/B/C/D × 六场景） | 内建断言：任一 cell 在 kNone 帧出现 Detector 调用即非零退出，全部通过 | **通过**（`evaluate_redetection_gate` kHoldStaticFrame 路径的实际效果） |

**"仅 D 通过"两项的口径论证**（不构成静默放宽）：

- **门槛值未变更**：`DEC-019` 第 5 条对 swap/假阳性两项按场景
  （`*-similar-icons`）定义达标线（≤ 0.05 / ≤ 0.02），未按方法定义达标线；
  本判定沿用 `M7-09` 报告已发布的判定口径（结论表"仅 D 通过/仅 D 严格
  通过"），不调低门槛数值、不改写任何测量。
- **判定口径 = D 的依据**：`DEC-019` 第 2 条冻结的证据模型是"外观
  （E1/E2）+ 位置-时间门控（含全局运动补偿）+ 语义兼容谓词 + impostor
  负模板"的全通道组合；harness 的 A/B/C 是为隔离各证据通道贡献而定义的
  调用方消融策略（基准报告"方法口径"节：A 无短路每帧全验证、B/C 无语义
  通道与 impostor 机制），不是可交付的能力配置。产品语义的跟踪能力 = D。
- **swap 差值归因**（基准报告核心发现 2）：孪生图标 E1 交叉 NCC 0.712
  落弱带 [0.6, 0.8)——无语义否决与负模板时（A/B/C）外观通道对同形异义
  对象结构性无判别力，各发生 2 次交换；D 经语义冲突否决（5 次）+ impostor
  负模板命中（4 次）实现零交换，代价为该对象 2 帧误判丢失后 2 帧内重捕获
  （`uncertain_frame_limit` 预算内）。A/B/C 的 0.5 恰好证明语义通道是
  被测的唯一防交换手段——机制按设计工作，而非门槛失败（`RISK-2026-16`
  门控证据）。
- **假阳性延续差值归因**：B/C 的 2/2 = 1.0 与 A 的 2/116 同源——无语义
  通道时弹出物被确认提交；D 的 0/1 为严格零。
- **回退现状**：两项风险在 D 配置下实测受控（零 swap、零假阳性延续），
  `RISK-2026-16`/`RISK-2026-17` 的回退（相似候选一律降级 `kUncertain`、
  收紧代际判定或位置通道降权）**不启用**；"是否将回退写入库默认"列为
  [DEC-022](../decisions/DEC-022-object-tracker-contract-freeze.md)
  开放项 3 交负责人裁决。

**库默认 `min_compensation_confidence` 裁定：维持 0.0，不随本判定上调
（无代码变更）。** 测量依据：

1. 判定范围：六项门槛的全部已发布判定数字（C/D 全部场景）均在 harness
   调用方配置 0.7 下采集（`M7-09` 校准表）；库默认 0.0 不出现在任何门槛
   判定路径，维持 0.0 不改变任何判定结论。
2. 置信度度量无真实数据先验（`DOD-05`）：实测分离带——真滚动 [0.93,
   0.95] vs 局部变化伪位移 [0.46, 0.55]、门 0.7 落宽分离带内——全部来自
   合成场景；`estimate_global_shift` 置信度在真实内容上的分布未测量
   （M7-04 交付记录："置信度与默认参数无真实数据先验"）。库默认是
   `DEC-022` 拟冻结的契约面：在仅有合成证据的时点把 0.7 固化为库默认，
   与"证据强度与承诺深度对齐"（`DEC-019` 第 4 条、`DEC-018` 立项逻辑）
   不一致——0.0（不过滤）是不虚构分离边界的无先验值，门控行为由调用方
   按其内容域配置。
3. 契约定位：`min_compensation_confidence` 的冻结语义是 `RISK-2026-17`
   的门控旋钮（M7-07）；跟踪管线为调用方组合帧管线，调用方持有
   `estimate_global_shift` 结果流并了解自身内容域。参考调用方策略
   （harness 0.7）已发布并被校准套件钉住
   （`HarnessCallerCompensationGate07SeparatesMeasuredBands`），集成方照
   配置即获得全部实测收益。
4. 如实记录不利面（维持 0.0 的代价）：0.0 门下 C/D 曾对局部变化帧（置信
   [0.46, 0.55] 伴随非零伪位移）平移整池（anim verify 归零异常，`M7-09`
   校准表实测）——未配置该旋钮的调用方存在已测量的伪位移暴露。处置：记
   为 [DEC-022](../decisions/DEC-022-object-tracker-contract-freeze.md)
   开放项 1——负责人可在其评审时裁决随真实数据评估一并复核库默认（与
   `peak_sidelobe_ratio_min` 5.0 复核同批），或在真实数据评估前以集成
   文档约定"调用方必配 0.7"。

判定限定（随结论一并留档，不得拆开引用）：

- **仅有合成证据**（`DOD-05`）：场景、纹理、遮挡与运动均为播种合成，E2
  结构测度由 harness 测量而非真实 `propose_regions` 输出、oracle Detector
  按 GT 粗召回。本判定证明"双通道 + 语义组合在合成场景可实现且达门槛"，
  不证明真实场景价值。
- **真实截图评估为转正前置**（`RISK-2026-14`）：与 `DEC-018` 阶段 2 共享
  `~/mirador-eval/` 采集（一次采集两用）；数据未采集不阻塞本合成口径
  判定，阻塞转正收口（`DEC-022` 阶段 2）。
- **`peak_sidelobe_ratio_min` 5.0 复核条件转记**：贫纹理合成 patch 实测
  真匹配 PSR 4.96 < 5.0 被拒（归因于刺激），场景纹理富化后真匹配 ≥ 6.95、
  杂峰 ≤ 3.5——真实数据评估必须复核该裕度（转记为
  [DEC-022](../decisions/DEC-022-object-tracker-contract-freeze.md) 开放
  项 2）。
- **环境口径**（`DEC-011`）：单机 Linux x64 release；物理 Android 设备与
  Windows 桌面缺失，补跑前结论限定 Linux x64。
- **swap 率量纲**为"每对象交换次数"（分母 = 场景对象数，随场景定义冻结，
  `evaluation-scenes` 约定）。

后续动作：

- [DEC-022](../decisions/DEC-022-object-tracker-contract-freeze.md)
  （Proposed）由负责人评审；批准前 M7 Experimental 面登记与标记不变。
- 本判定不改变 M7-11~13 的计划性交付路径（`DEC-019` 第 6 条、
  [DEC-020](../decisions/DEC-020-tracker-backend-spi.md) 已 Accepted：深度
  增强通道为可选注入，`A/B/C/D` 基准追加"D+深度增强"口径列报，不阻塞
  亦不被本判定约束）。
- 兼容性登记与 API 索引已按两阶段模式注记（本判定 + `DEC-022` Proposed；
  批准前 Experimental 口径不变）；CHANGELOG Unreleased 已登记本判定条目
  （2026-09-28），`v0.4.0` 发布说明整理仍随 M7 收尾统一处理。
- 真实截图评估采集与三项开放项裁决随 `DEC-018` 阶段 2 协调排期。

## 测试与退出条件

- [ ] 全部工作项完成，6 预设（debug/release/warnings/asan/ubsan/tsan）构建
  与 ctest 通过，lint 双口径归零（同 M6 收口口径）。
- [ ] 架构测试证明核心链接闭包不变（跟踪实现全部在 fusion/image 既有闭包
  内，零新依赖）。
- [ ] 确定性测试：同输入帧序列产生同状态序列、同 ID 结果、同 trace。
- [ ] 预算负向测试：目标数/模板/历史超限显式淘汰并上报，无静默增长
  （`RULE-06`）；模板版本/参数变化使验证失效（`DOD-04`）。
- [ ] 状态机边界：`uncertain_frame_limit` 超时转 `kLost`、代际切换降级、
  `kTerminated` 失败可见、重捕获延续/新 ID 两分支。
- [ ] 补偿坐标矩阵：0/90/180/270 度、奇数尺寸、非连续 stride、往返容差
  （`DOD-03`）。
- [ ] 重检测负向：静止画面零触发；退避序列符合配置；预算耗尽显式失败。
- [ ] `TrackerBackend` SPI（`DEC-020`）：Fake 后端编排测试覆盖初始化/更新/
  失败降级/句柄析构/取消 deadline；跟踪会话状态不进能力结果缓存的负向
  测试；未注入深度通道时传统管线行为零变化的对照测试。
- [ ] NanoTrack 参考后端：`integrations` 套件合成模型冒烟通过；`docs/
  supply-chain/` 许可证与来源登记完成；默认构建零获取口径核实。
- [ ] 隐私负向（`DOD-06`/`RULE-10`）：跟踪路径不落盘、不联网、trace/日志
  不含模板内容与原始帧。
- [x] M7-09 基准发布且门槛逐项判定；M7-10 判定记录完成（**GO（合成口径）**，
  见上方"Go/No-Go 判定记录"节；转正草案
  [DEC-022](../decisions/DEC-022-object-tracker-contract-freeze.md)
  Proposed）。
- [ ] 文档同步：API 索引（Experimental 节）、兼容性登记、CHANGELOG、总计划
  `SCOPE-13`、设计文档 §24 M7 状态。

## 验证记录

2026-09-21：M7 立项生效与 `M7-01` 交付（分支 `feat/m7-cross-frame-object-tracking`；
测试由 Independent-Verification-Agent 独立编写与执行）：

- 立项：负责人指示"依照设计与计划，继续下一阶段开发"——[DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md)
  与 [DEC-020](../decisions/DEC-020-tracker-backend-spi.md) 同轮批准转
  Accepted，[跟踪设计](../design/object-tracking-design.md)转 Active，本里程碑
  转 In Progress；总计划 1.8 修订。
- `M7-01`：公共契约 `include/mirador/object_tracker.hpp`（`TrackState` 四态、
  `EvidenceGrade` 四级、`TrackObservation`/`TrackTemplate`/`TrackSemantics`、
  `TargetTrack`、`ObjectTrackerOptions`、`TrackAdoption`、`ObjectTracker`）与
  `src/fusion/object_tracker.cpp`。全部默认值冻结：`max_targets` 64、
  `max_position_history` 32、`max_templates` 4、`max_negative_templates` 4、
  `template_thumb_side` 32、`pool_budget_bytes` 1 MiB、`uncertain_frame_limit` 5、
  `max_generation_lag` 1、验证阈值初值 NCC 强 0.8/弱 0.6、峰旁瓣比 5.0、结构
  偏差容差 0.2、验证 ROI 对角比例 1.0、重检测退避 1/60 帧、最大尝试 8。
  `terminate` 归档释放模板/负模板/位置历史，仅保留身份记录（失败可见）。
  池预算、显式淘汰（terminated 优先 → 最旧验证 → 低 id）与失败原子性
  （错误路径池不变）为冻结语义。帧级管线方法（门控短路/邻域验证/运动补偿/
  级联重检测原语）随 `M7-03`~`M7-08` 在同一 Experimental 头内扩展
  （`PerceptionSession` M2 建类、M4 增 `fuse()` 的既有演进先例）。
- 验证：`mirador.fusion.object_tracker` 35 用例覆盖 create 校验矩阵、adopt
  错误原子性、covering-ROI 模板指纹与公共 API 逐位一致、字节记账公式、
  显式淘汰四规则、字节预算压力、terminate 生命周期、reset、0/90/180/270
  旋转 × 奇数尺寸 × 非连续 stride × 贴边坐标矩阵（`DOD-03`）与双 tracker
  确定性。首轮发现 2 处实现缺陷（terminate 字节记账多减 history/语义、
  精确满池时捕获预算阻塞淘汰）与 1 处契约歧义（归档是否保留位置历史，
  裁决为释放），修复后复验通过。六预设：debug 45/45（`mirador.adapters.opencv`
  为 debug 既有条目差异）、asan/ubsan/tsan/release/warnings 各 44/44，
  ASAN/UBSAN/TSAN 零告警；clang-format 三文件零违规；clang-tidy
  `--warnings-as-errors='*'` 实现文件退出码 0；架构与隐私测试随全量套件通过。
- 同步：API 索引（fusion 节 Experimental 条目）、兼容性登记新增
  Experimental API 节（不计兼容性承诺）、CHANGELOG Unreleased、设计 §5
  冻结落点注记、总计划 1.8 修订与里程碑索引。
- CI 回填：PR #20（[run 35626923647](https://github.com/Linductor-alkaid/mirador/actions/runs/35626923647)）
  14/14 job 全绿——msvc/ninja、ndk/arm64-v8a、gcc10（focal 容器）、clang debug/fuzz、
  integrations-ncnn、capture/opencv 适配与 clang-format/clang-tidy 双口径。
  首轮 CI lint 在测试文件暴露 11 处违规（断言辅助函数认知复杂度、const/qualified-auto、
  optional 解引用、include-cleaner；此前 tidy 检查只覆盖了实现文件），按仓库先例
  拆分辅助函数修复（35 用例名称、数量与断言语义不变），复跑后全绿。
- 限制：阈值初值为开发冒烟默认值，M7-09 校准（`DEC-019` 第 5 条）。

2026-09-22：`M7-02` 目标池有界结构交付（实现于主循环，测试由
Independent-Verification-Agent 独立编写与执行）：

- 交付：`ObjectTracker` 池有界变更原语——`record_observation`（有界位置历史
  追加：当前池代际打戳、confidence 钳制、溢出显式淘汰最旧并计入
  `evicted_observation_count`；纯簿记，不触碰 `state`/`last_bounds`/
  `predicted_center`/`confidence`/`last_verified_sequence`——证据级确认更新
  留给 M7-06 状态机）、`add_template`（模板集存储：index 0 初始模板钉死，
  溢出淘汰最旧非初始模板并计数，`max_templates == 1` 无可淘汰显式失败）、
  `add_negative_template`（负模板存储：无钉死项淘汰最旧，容量 0 显式
  kBudgetExceeded）、`advance_layout_generation`（代际确定性递增，
  uint32 耗尽显式失败）、`observations_in_generation`（按代际分组的历史
  查询，设计 §6.4）与三个淘汰 trace 计数器（`reset` 清零；`terminate` 与
  adopt 整 track 淘汰不计入——前者是调用方行为，后者只计入
  `evicted_track_count`）。全部路径维持 M7-01 冻结语义：字节预算公式不变、
  错误路径池完全不变、淘汰显式可见（`RULE-06`：不静默增长、不静默丢弃）。
  分组采用"平铺有界存储 + 按代际过滤访问"实现（历史默认 32 条，线性过滤
  成本可忽略），不改 `TargetTrack` 已冻结的数据布局。
- 验证：`mirador.fusion.object_tracker` 21 个新用例（56/56）覆盖追加/打戳/
  钳制、校验矩阵（unknown/terminated/非有限/宽高 ≤ 0/指纹三维不匹配）、
  溢出淘汰与计数器、字节满池两分支（容量到顶走字节中性交换 vs 未到顶显式
  拒绝）、校验先于淘汰（满容量下非法条目不触发淘汰）、跨代分组与跳代查询、
  reset 计数清零、错误穿插的双实例确定性与逐字节核算交叉校验。六预设
  ctest：debug 45/45、release/warnings/asan/ubsan/tsan 各 44/44（ctest 按
  二进制注册；二进制内 gtest 35 → 56），object_tracker 在 asan/ubsan/tsan
  直跑 56/56 且 sanitizer 零报告；clang-format 全仓归零（首轮一处 getter
  未合并单行，已修）、clang-tidy `--warnings-as-errors='*'` 92 文件退出码 0。
- 限制与衔接：`kLost`/`kUncertain` 状态下的记录路径暂不可经公共 API 构造
  （状态机随 M7-06 落地后补测；实现检视确认门禁仅拒绝 `kTerminated`）；
  `advance_layout_generation` 的 uint32 耗尽分支无法实际注入，未测。代际
  推进的触发判定（全局变化分类）与代际切换降级随 M7-07/M7-06 交付。
- CI 回填：PR #21（[run 35685102913](https://github.com/Linductor-alkaid/mirador/actions/runs/35685102913)）
  14/14 job 全绿——msvc/ninja、ndk/arm64-v8a、gcc10（focal 容器）、clang
  debug/fuzz、integrations-ncnn、capture/opencv 适配与
  clang-format/clang-tidy 双口径；首轮通过，无修复往返。

2026-09-22：`M7-03` 变化检测门控三级短路交付（分支
`feat/m7-03-change-gated-short-circuit`；实现与基准已合入工作分支，测试由
Independent-Verification-Agent 独立编写与执行，六预设门禁与 CI 证据按仓库
先例回填）：

- 交付：`ObjectTracker::evaluate_change_gate`（Experimental，帧级管线首个
  方法落 `object_tracker.hpp`，兑现 M7-01 冻结注记）与配套类型
  `ChangeGateDecision`/`TrackGateDecision`/`ChangeGateTrace`。门控消费调用
  方（session 主循环）跑出的 M1 `detect_change` `ChangeReport`，tracker 不
  自持上一帧、不重复实现变化检测；输出按 track_id 升序（池确定性枚举序）
  的逐 track 决策：`kNone` 全部 `kReuse`（零逐 track 几何计算，近零路径）、
  `kPartial` 逐 track 变化 ROI × `last_bounds` 相交判定（`predicted_center`
  在 M7-04/M7-07 前恒为 `last_bounds` 中心；不相交 `kReuse`、相交 `kVerify`
  并携带首条相交 ROI 扫描序索引）、`kGlobal` 全部 `kVerify` 不短路。契约
  裁决：门控为纯决策（`const`，任何路径不改池状态），短路复用不推进
  `last_verified_sequence` 等证据字段（位置先验非外观证据，设计 §3；证据
  级确认随 M7-06，历史簿记经 `record_observation` 留给调用方，故方法不收
  `frame_sequence`）；非 `kTracking` 态显式 `kInactive` 不静默跳过；
  `kGlobal` 触发的 `advance_layout_generation` 判定仍归 M7-07（禁止提前）；
  相交判定复用 `src/fusion/rect_math.h` 既有工具，O(tracks × ROIs) 有界，
  错误路径池天然不变；kCancelled/kTimeout 经 `ExecutionContext` 显式转化
  （入口 + kPartial 逐 track 检查，取消只返回 Status 不返回半份 trace）；
  滚动类变化全员进入验证入口为本阶段预期行为（`RISK-2026-17`，补偿随
  M7-04/M7-07）。
- 基准对照（M7-03 特有验收）：新增 `benchmarks/change_gate_bench.cpp`
  （`mirador_bench_change_gate`，M1 基准同口径，场景分类与 verify 计数由
  基准自身断言）。Linux x64 release（`DEC-011` 口径）：gate-only p50
  0.09–0.34 µs（三次运行、全部三级路径稳定），detect+gate 与 detect 单独
  计时差值在运行噪声内——三级短路路径相对 M1 `detect_change` 基线无可测
  回归；数字与口径限定发布于
  [docs/benchmarks/linux-x64-change-gate-2026-09.md](../benchmarks/linux-x64-change-gate-2026-09.md)。
- 本地验证（交付时点）：debug 预设构建零告警通过；门控基准四场景断言
  通过（kNone→9 reuse / partial-disjoint→9 reuse / partial-hit→2 verify
  +7 reuse / kGlobal→9 verify）；`mirador.fusion.object_tracker` 既有 56
  用例 debug 直跑通过；clang-format 全仓 dry-run 零违规；clang-tidy
  `--warnings-as-errors='*'` 对本次两个改动源文件（`object_tracker.cpp`、
  `change_gate_bench.cpp`）退出码 0（首轮 8 处：认知复杂度 41>25 拆分
  辅助函数、include-cleaner ×2、C 数组 ×2、多余拷贝、显式宽化 ×2，
  均已修）。
- 文档同步：头文件契约注释（三级语义、证据字段不动与
  `last_verified_sequence` 裁决、坐标空间、确定性与错误语义）、设计 §6.1
  M7-03 冻结落点注记、API 索引 fusion 节、兼容性 Experimental 登记、
  CHANGELOG Unreleased、总计划 1.10 修订、基准报告新档。
- 限制与衔接：邻域验证器本体随 M7-05，`kVerify` 仅为显式入口，门控不伪造
  验证结果；`kUncertain`/`kLost` 态仍不可经公共 API 构造（M7-02 限制段延
  续），门控对其的 `kInactive` 路径与 `kTerminated`（经 `terminate` 可构造）
  同一实现分支，M7-06 状态机落地后补齐两态的构造级测试；同帧多 track 独
  立短路、DOD-03 坐标矩阵（0/90/180/270 × 奇数尺寸 × 非连续 stride × 贴
  边）与取消/超时转化由 Independent-Verification-Agent 测试覆盖（门控为
  纯 `RectF` 判定，不涉 stride/方向重采样，矩阵按坐标入口覆盖）；六预设
  ctest、sanitizer 与 CI 证据待回填后勾选工作项。

2026-09-23：验证员发现 1 处低严重度契约注释与实现不符——头注释声称门控
"无返回 trace 之外的分配"，而 kPartial 成功路径经 `float_rois` 分配临时
`std::vector<RectF>`（受 report ROI 数有界，无功能/资源上限影响）。处置：
不放宽契约，改为逐 ROI 就地转换（标量 int→float，精确）消除临时量、删除
`float_rois` 辅助，头注释措辞改为"分配限于返回 trace 与错误路径 Status
消息"（错误 Status message 本身有字符串分配，如实交底）。本地复验：debug
构建零告警、`mirador.fusion.object_tracker` 全部 72 用例（含验证套件
16 用例）通过、两改动文件 clang-format 归零、clang-tidy
`--warnings-as-errors='*'` 对 `object_tracker.cpp` 退出码 0；门控基准复测
gate-only p50 0.199–0.240 µs 仍落原 0.09–0.34 µs 区间（基准报告表已换为
修复后 run）。同日本地门禁另发现 tsan 预设 `ctest` 概率性失败（exit 8，
35/44）：定位为高熵 ASLR 内核（`vm.mmap_rnd_bits = 32`）下 TSAN shadow
gap 与随机化库映射冲突，与 M7-03 代码无关（`setarch -R` 包装下 44/44 稳
定通过）——修复为 Linux/TSAN 构建的测试注册自动经 `setarch -R` 启动每个
测试进程（`build(tests)` commit），裸 `ctest --preset tsan` 三连跑
44/44，CI 的整体包装保留。

2026-09-23：`M7-03` 测试与门禁证据落地，工作项勾选（分支
`feat/m7-03-change-gated-short-circuit`；验证套件由
Independent-Verification-Agent 独立编写与执行，同日契约修正与 tsan 门禁
修复见上段）：

- 验证覆盖：`mirador.fusion.object_tracker` 新增 16 个 M7-03 用例（二进制
  56 → 72）——三级分类逐场景断言（kNone 全复用、kPartial 贴边 ROI 短路/
  相交携带首条扫描序索引/同帧多 track 独立决策、kGlobal 全验证）、空池
  回显分类、非 `kTracking` 态显式 `kInactive`、纯决策不改池且不推进代际、
  双实例逐位确定性、DOD-03 坐标矩阵（0/90/180/270 × 奇数尺寸 × 非连续
  stride × 贴边）、非法 `ChangeReport` 拒绝且池不变、取消/超时显式转化
  （入口超时、取消优先且不发布半份 trace、kPartial 扫描中取消中止）、
  trace 有界（池上限 × ROI 数）与端到端消费 `detect_change` 报告。
- 门禁：debug 预设 ctest 45/45；tsan 预设经上段 `setarch -R` 注册修复后
  裸 `ctest --preset tsan` 44/44；`mirador.fusion.object_tracker` 直跑
  debug/release/asan/ubsan/tsan（tsan 经 setarch 包装）各 72/72 且
  sanitizer 零报告；clang-format 全仓 dry-run 归零；clang-tidy
  `--warnings-as-errors='*'` 对 `object_tracker.cpp` 与
  `object_tracker_test.cpp` 退出码 0。门控基准复跑（1280x720、9 tracks、
  300 迭代）：gate-only p50 0.198–0.244 µs 落已发布 0.09–0.34 µs 区间，
  detect+gate 与 detect 单独计时差值在噪声内，四场景分类与 verify 计数
  自断言通过——相对 M1 基线无可测回归结论维持。
- 限制：`build(tests)` 修复仅改 TSAN 分支的测试注册命令（非 TSAN 预设
  注册零差异），debug/tsan 完整 ctest 与五构建 object_tracker 直跑已在
  修复后复验；asan/ubsan/warnings 预设的完整 ctest 由 CI 在包含该修复
  的分支 head 上重跑通过（见下方 CI 回填；CI 矩阵无 release 预设，release
  侧证据为本地 object_tracker 直跑与门控基准复跑）。
- CI 回填：PR #22 两轮 run 全绿，14/14 job——msvc/ninja、ndk/arm64-v8a、
  gcc10（focal 容器）、gcc debug/asan/ubsan/tsan/warnings 五预设、clang
  debug/fuzz、integrations-ncnn、capture/opencv 适配与 clang-format/
  clang-tidy 双口径。[run 35756793022](https://github.com/Linductor-alkaid/mirador/actions/runs/35756793022)
  （head b11b70d，覆盖验证套件、契约修正与 `setarch -R` 注册修复）先行
  通过；分支 head [run 35759053505](https://github.com/Linductor-alkaid/mirador/actions/runs/35759053505)
  （head 3a2cb3c，仅追加文档回填 commit，34m30s）复证全绿；首轮通过，
  无修复往返。

2026-09-23：`M7-04` 全局位移估计原语实现交付（分支
`feat/m7-04-global-shift-estimation`，自 master d96f1af 切出；实现于主
循环，测试按分工由 Independent-Verification-Agent 独立编写与执行，工作项
勾选随验证套件与门禁证据落地后回填）：

- 交付：公共契约 `include/mirador/shift_estimation.hpp`（Experimental，
  随 M7 go/no-go 冻结）与 `src/image/shift_estimation.cpp`，源文件注册进
  根 CMakeLists.txt 的 image 目标。`estimate_global_shift` 双入口：
  `ImageView` 双帧与 M1 `ChangeSignature` 双签名（直接检索存储缩略图，
  与视图版在同尺寸下逐位一致，零分配）；变化检测的确定性灰度缩略图管线
  （整数 area 重采样 + BT.601 luma）抽取为内部头 `src/image/gray_thumbnail.h`
  供两处共享（行为不变重构）。语义决策冻结于头注释：搜索 =
  `[-max_shift, max_shift]²` 整数平移全集 × 固定中心比较窗（每候选等像素
  数，工作量与帧尺寸无关，`RULE-06`）；胜者取总序 (SAD, 切比雪夫半径,
  dy, dx)；置信度 = 峰显著度 `(μ_others − best)/(μ_others + best)`
  （单候选/全平手为 0，缩略图分辨率逐像素相同为 1；整数和 + 单次 double
  除法，无随机/浮点平台差异路径）；位移精度 = 精确有理数
  `thumbnail_shift × frame_dim / thumbnail_size`（double 求值收窄
  float），方向 p_curr = p_prev + (dx, dy)，经 `make_translation` 进
  `Transform2D` 组合链（`RULE-05`，DOD-03 矩阵适用）；两帧须同呈现尺寸，
  比较恒在呈现像素上进行（同 `detect_change` 约定）；预算字段
  `work_budget_bytes` 逐内部分配请求检查，超限显式 `kBudgetExceeded`；
  搜索为有界非平凡循环，经 `ExecutionContext` 入口 + 逐行检查显式转化
  kCancelled/kTimeout（校验先于取消），不返回半份结果。纯函数：不触碰
  `ObjectTracker` 池状态、不做变化分类与代际判定（M7-07 边界维持，
  `RISK-2026-17` 的补偿效果随 M7-09 A/B/C/D 矩阵门控，本项不发布性能
  承诺）。默认参数（thumbnail 64 / max_shift 16 / 512 KiB）为开发冒烟
  值，M7-09 校准（`DEC-019` 第 5 条）。
- 本地验证（交付时点）：debug 预设构建零告警；debug ctest 45/45（缩略图
  管线重构后既有套件行为不变）；开发冒烟自检（临时脚本，不入仓）覆盖
  同帧零位移 + 置信度 1、已知整数位移恢复（+8, −4 帧像素 → 缩略图
  (+4, −2)、帧 (8.0, −4.0)）、签名版与视图版逐位一致、双实例逐位确定、
  参数校验矩阵、1 字节预算显式 `kBudgetExceeded`、呈现尺寸不匹配拒绝、
  取消/超时显式转化与 NV12 luma 路径；clang-format 全仓 dry-run 零违规；
  clang-tidy `--warnings-as-errors='*'` 对 `shift_estimation.cpp` 与
  `change_detection.cpp` 退出码 0。六预设 ctest、DOD-03 坐标矩阵
  （0/90/180/270 × 奇数尺寸 × 非连续 stride × 贴边/往返容差）、确定性/
  预算/隐私负向测试与 CI 证据待验证套件落地后回填。
- 限制：位移估计质量仅声明合成口径（`DOD-05`：真实场景不宣称，M7-09
  基准与真实数据评估收口）；置信度与默认参数无真实数据先验。

2026-09-23：验证员首轮发现 `M7-04` 三项问题，实现侧修复与契约注释精度
同步（契约矩阵负向用例由验证员在其套件内补充；本段为实现侧处置记录）：

- 高（内存安全 + 契约违约）：`estimate_global_shift` 签名重载缺少两签名
  存储缩略图的尺寸一致性校验——契约明文承诺尺寸不一致返回
  kInvalidArgument，实现只逐签名校验方形 [8, 256]，`search_shift` 的窗口
  索引导出仅取 previous 缩略图边长：前缩略图大于当前缩略图时堆越界读
  （ASAN 实证：64x64/8x8 签名、forged frame dims 均 256x256、
  max_shift=8，`heap-buffer-overflow READ of size 1 at window_sad ←
  search_shift ← estimate_global_shift`），反向（前小后大）则静默接受并
  产生无意义结果。修复：两个 `validate_signature` 之后、取消检查之前补
  宽高相等校验（`detect_change` 签名重载同款检查），公共契约未放宽；
  校验为纯标量比较，签名重载"零分配"语义不变。
- 低（语义文档歧义）：冻结的置信度公式在"胜者 SAD=0 且另有候选并列 0"
  （周期内容位移恰为一个周期）时仍输出恰 1.0——实现与冻结公式一致、
  胜者总序仍唯一，非代码缺陷；头注释补并列零候选告警：
  confidence == 1.0 不得解读为无歧义峰，确定性由总序（最小切比雪夫
  半径，其次 dy/dx）保证，供 M7-07 消费侧与 M7-09 校准知悉。
- 低（预算口径文档精度）：头注释原称预算覆盖"重采样中间产物 + 灰度
  缩略图且不分配任何其他内存"，而 `resize_area` 内部按源尺寸分配两个
  int64 box 权重表（上限约 2 × 65535 × 8B ≈ 1 MiB），不经
  `work_budget_bytes` 检查、仅分配失败映射 kBudgetExceeded——为 M1 起
  既有共享路径（`detect_change` 同），非本项回归，任意输入尺寸的固定
  上界（`RULE-06`）仍成立；头注释措辞改为如实交底权重表口径，
  `resize_area` 侧是否单独立项收口留待后续决策。
- 复验（实现侧本地证据）：修复前 ASAN 复现与验证员报告逐帧一致；修复后
  探针（64→8 与 8→64 双向 kInvalidArgument、等尺寸路径零位移不变、ASAN
  零报告）通过；debug 预设构建零告警；debug ctest 46/46（含验证套件
  `mirador.image.shift_estimation` 全部既有用例）；clang-format 两改动
  文件归零；clang-tidy `--warnings-as-errors='*'` 对 `shift_estimation.cpp`
  退出码 0；首轮开发冒烟复跑全过。六预设 ctest 与 CI 证据仍随验证套件
  门禁落地回填。

2026-09-23：`M7-04` 测试与门禁证据落地，工作项勾选（分支
`feat/m7-04-global-shift-estimation`；验证套件由 Independent-Verification-Agent
独立编写与执行，实现交付与同日验证员三项发现的处置见上两段）：

- 交付摘要：`estimate_global_shift` 双入口公共契约
  `include/mirador/shift_estimation.hpp`（Experimental）与
  `src/image/shift_estimation.cpp`（语义冻结见本日交付段）；本轮补齐签名
  重载缩略图尺寸一致性校验（d7bc29e，契约未放宽）并同步头注释精度
  （3d12763）；尺寸一致性负向用例随套件补齐
  （`SignatureOverloadRejectsMismatchedThumbnailSizes`，commit 01642e8，
  前缩略图更大方向即 ASAN 实证越界读的回归锁定）。
- 验证覆盖：`mirador.image.shift_estimation` 23 用例（commit 8862182 的
  22 个 + 回归 1 个）——已知整数位移恢复与冻结的置信度/精度规则（逐轴
  映射与精确有理数 float 收窄）、胜者总序（切比雪夫半径与周期并列）、
  重复调用/独立帧拷贝/签名重载逐位确定性、DOD-03 坐标矩阵（旋转元数据
  × 奇数 33x21 × 非连续 stride × max_shift 贴缩略图边）、极窗边界位移、
  预算边界显式 `kBudgetExceeded`、错误模型矩阵、入口/搜索中途 kCancelled
  与 kTimeout 显式转化、gray/RGBA/NV12 格式路径逐位一致、签名重载坏状态
  拒绝（含尺寸不一致双向）与隐私默认零落盘（`DOD-06`）。
- 门禁：debug 预设 ctest 46/46、`mirador.image.shift_estimation` 直跑
  23/23；asan/ubsan/tsan 预设全量 ctest 各 45/45（debug 多 1 条为
  `mirador.adapters.opencv` 既有条目差异），sanitizer 零报告，tsan 经
  `setarch -R` 注册包装裸 `ctest --preset tsan` 通过；clang-format 全仓
  dry-run 归零；clang-tidy `--warnings-as-errors='*'` 对
  `shift_estimation.cpp` 与 `shift_estimation_test.cpp` 退出码 0。修复前
  ASAN 探针复现与修复后双向 `kInvalidArgument` 复验见上段（探针为会话内
  复验工具未入仓，场景已由回归用例永久化）。
- 限制：release/warnings 预设完整 ctest 未在本轮执行——warnings 侧已由
  CI 在分支 head 覆盖（见下方 CI 回填；CI 矩阵无 release 预设），六预设
  完整门禁仍随编排脚本收口（同 M7-03 先例）；位移估计质量仅合成口径
  （`DOD-05`），置信度与默认参数先验随 M7-09 校准；`resize_area` 权重表
  预算口径是否单独立项收口留待负责人决策（见上段处置记录）。
- CI 回填：PR #24 单轮 run 全绿，14/14 job——msvc/ninja、ndk/arm64-v8a、
  gcc10（focal 容器）、gcc debug/asan/ubsan/tsan/warnings 五预设、clang
  debug/fuzz、integrations-ncnn、capture/opencv 适配与 clang-format/
  clang-tidy 双口径。[run 35808507168](https://github.com/Linductor-alkaid/mirador/actions/runs/35808507168)
  （head 58fabd4，覆盖实现、验证套件、签名重载尺寸一致性修复与文档回填
  commit，30m30s）；首轮通过，无修复往返。

2026-09-23：`M7-05` 邻域验证器实现交付（分支
`feat/m7-05-neighborhood-verifier`，自 master fce42ab 切出；实现于主循环，
测试按分工由 Independent-Verification-Agent 独立编写与执行，工作项勾选随
验证套件与门禁证据落地后回填）：

- 交付：`ObjectTracker` 邻域验证器——公共契约与配套类型
  `TrackStructureDescriptors`/`AppearanceChannelOutcome`/
  `StructureChannelOutcome`/`AppearanceVerification`/`StructureVerification`/
  `TrackVerification`，方法 `verify_track`（纯逐 track 双通道证据决策，
  `const`：不改池状态、不推进 `last_verified_sequence`，分级→状态转移与
  代际判定归 M7-06，同 M7-03 先例）、验证 ROI 纯查询 `verification_roi`
  与 E2 基线簿记 `record_structure_baseline`；新选项
  `verification_work_budget_bytes`（默认 256 MiB 开发冒烟值，M7-09 校准）。
  四项契约裁决冻结于头注释：(1) 验证器形状为纯决策，E2 消费裸描述量
  `TrackStructureDescriptors` 而非 `GeometricRegionProposal` 类型——
  `mirador_fusion` 链接接口保持恰为 core/image/cache，零新依赖（`DEC-019`
  第 1 条阶段 A 口径，架构测试与链接闭包探针不变），调用方经
  `verification_roi`（与验证器同一 ROI 规则）在验证 ROI 内运行线段检测 +
  `propose_regions` 后传入；(2) E2 基线经 `record_structure_baseline` 入池
  （每 track 单槽、后写覆盖、`kStructureBaselineOverheadBytes = 32` 计入
  `byte_size`/`pool_budget_bytes`，放不下显式 `kBudgetExceeded`；
  `terminate`/track 淘汰/`reset` 释放；`TargetTrack` 冻结布局不动，槽位为
  池侧并行存储），偏差口径 |q − 基线_q| / max(|基线_q|, 1e-6) 三量取最大
  对比 `structure_deviation_tolerance`，偏差不截断上报供 M7-09 校准；(3)
  负模板边界：验证器只读正模板，impostor 采集与 `kVetoed` 否决归 M7-06；
  (4) 验证 ROI：`last_bounds` 以 `verification_roi_diagonal_ratio × 对角/2`
  每侧围绕 `predicted_center` 扩展，adopt_track 覆盖规则取整并钳制到视图，
  钳制后小于窗口时退化为仅评估 (0,0) 偏移（`best_offset_* == 0` 可见）。
  E1：ROI 内"窗口完全落在 ROI 内"整数平移全集 × 全部正模板，adopt_track
  同款管线（crop + M3-09 fingerprint）提取候选、与 VisualIndex 模板层
  （M3-10）同一归一化 NCC；逐模板响应面峰取总序（峰值 → 切比雪夫半径 →
  dy → dx），PSR = (峰 − 旁瓣均值)/(旁瓣总体标准差 + 1e-12)（平坦面 0、
  单候选集峰/1e-12），胜者模板总序（峰值 → PSR → 模板序）；kStrong 要求
  峰值与 PSR 同时达标，PSR 不达标峰高一律不采信。非 `kTerminated` 态均可
  验证（M7-08 身份复核复用），kTerminated 显式 kInvalidArgument（M7-02
  语义）。E1 规划工作量（饱和算术）先于像素读取对比工作预算，超限显式
  `kBudgetExceeded`；取消/超时入口 + 逐行检查、校验先于取消、不返回半份
  结果；同输入逐位确定（固定扫描序 + 总序 + 精确整数和上的单次除法）。
- 本地验证（交付时点）：debug 预设构建零告警；debug ctest 46/46（既有
  `mirador.fusion.object_tracker` 72 用例直跑通过，含 adopt/terminate/
  reset/字节记账改动路径回归）；开发冒烟自检（临时脚本，不入仓）60 余项
  断言通过——同帧验证 kStrong（峰 1.0/PSR 16.8）、平坦场景峰 1.0 但
  PSR≈0 判 kNone（平坦性拒绝）、(8,4) 位移恢复为峰偏移、模板集变化使
  新模板成为最优（`DOD-04` 负向）、容差边界两侧分级不同（参数变化使验证
  失效）、基线簿记字节记账（+32/覆盖中性/terminate 释放/放不下显式
  kBudgetExceeded 且池不变）、取消/超时/校验先于取消、错误模型矩阵、双
  实例逐位确定；clang-format 全仓 dry-run 归零（两改动文件）；clang-tidy
  `--warnings-as-errors='*'` 对 `object_tracker.cpp` 退出码 0（首轮 7
  处：OffsetGrid 成员函数触发 non-private-member 与 verify_track/
  adopt_track 认知复杂度超限，按 M7-03 先例拆分
  `plan_eviction`/`planned_scan_work`/`scan_appearance_responses`/
  `appearance_verdict`/`structure_verdict` 辅助后归零）。六预设 ctest、
  sanitizer、DOD-03 坐标矩阵、`DOD-06` 隐私负向与 CI 证据待验证套件落地
  后回填。
- 限制与衔接：阈值与预算初值无真实先验（`DEC-019` 第 5 条，M7-09 校准，
  `DOD-05` 不宣称真实场景效果）；`kUncertain`/`kLost` 态仍不可经公共 API
  构造（M7-02 限制段延续），其验证路径与 M7-06 状态机落地后补构造级测
  试；E2 描述量按 proposal 契约假定 [0, 1]（越界显式拒绝），真实
  `propose_regions` 输出的端到端联测随 M7-09 harness；分级→状态转移、
  高置信模板更新与 impostor 否决归 M7-06。

2026-09-23：验证员首轮发现 `M7-05` 一处出处引用不准确与三项记录级知悉，
实现侧处置（注释精度同步，行为与冻结契约不变；验证套件 30 用例由验证员
随 test(fusion) commit 落地）：

- 轻微（文档出处）：`verify_track` 头注释称校验先于取消为"(M7-02
  semantics)"，而 M7-02 实际冻结语义相反——`adopt_track` 入口先查取消/
  超时再做校验（src/fusion/object_tracker.cpp:532-537），且
  `AdoptCancelledContextTakesPriorityOverValidation`
  （tests/fusion/object_tracker_test.cpp:468-484）把取消优先钉死为 M7-02
  行为。M7-05 实现本身（校验 → 预算 → 取消）符合本工作项验收口径
  「校验先于取消」，验证套件亦按文档化语义钉死
  （object_tracker_verification_test.cpp："validation and budget errors
  beat cancellation"），行为不改；头注释与实现内联注释的出处更正为
  「M7-05 本项冻结决策」，并显式注明与 `adopt_track` M7-02 取消优先入口
  的刻意对照，避免 M7-06/M7-08 引用混乱。
- 信息（不可达防御分支）：头注释错误列表中 "a track with no appearance
  templates" 经公共 API 不可达（kTerminated 在更早处被拒绝、adopt_track
  恒存恰好 1 个模板）——纯防御分支行为正确、测试无法构造，头注释就地
  标注 "defensive — unreachable through the public API"。
- 信息（工作量计量口径）：冻结的 planned-work 公式以 ceil(bounds) 估算
  窗口字节，逐偏移 crop 实际按 covering 规则取整，分数边界下可比计量值
  每偏移多读至多一像素行/列——按契约实现（公式即冻结计量口径），头注释
  补 metering-precision 注记，余量归 M7-09 校准。
- 信息（ROI 精度口径）：`clamped_verification_roi` 将 double margin 收窄
  为 float 后展开，ROI 边缘相对全 double 计算可有 ±1 像素差异；确定性
  不受影响（同输入逐位同结果已测），`verification_roi` 头注释补
  precision note（M7-09 以真实阈值校准 ROI 覆盖率时的冻结计量口径）。
- 复验（实现侧本地证据）：debug 预设重建零告警；debug ctest 47/47
  （`mirador.fusion.object_tracker` 72 用例与验证套件
  `mirador.fusion.object_tracker_verification` 30 用例直跑均通过）；两
  改动文件 clang-format dry-run 归零；clang-tidy
  `--warnings-as-errors='*'` 对 `object_tracker.cpp` 退出码 0。

2026-09-23：`M7-05` 测试与门禁证据落地，工作项勾选（分支
`feat/m7-05-neighborhood-verifier`；验证套件由 Independent-Verification-Agent
独立编写与执行，实现交付与验证员首轮四项发现的处置见上两段）：

- 交付摘要：`ObjectTracker::verify_track`（纯逐 track 双通道证据决策）、
  验证 ROI 查询 `verification_roi`、E2 基线簿记 `record_structure_baseline`
  与新选项 `verification_work_budget_bytes`（语义冻结见本日交付段）；
  验证员首轮处置以注释级修改落地（1210d32：校验/取消优先级出处更正为
  本项冻结决策并显式注明与 `adopt_track` M7-02 取消优先入口的刻意对照，
  另补不可达防御分支与工作量/ROI 计量精度注记，公共契约面零变化）；
  30 用例验证套件 `tests/fusion/object_tracker_verification_test.cpp`
  随 test(fusion) commit 8a2e859 落地。
- 验证覆盖：`mirador.fusion.object_tracker_verification` 30 用例——
  `verification_roi` 冻结扩展规则/贴边钳制/非法输入拒绝、同帧零偏移
  kStrong、整数位移峰偏移恢复、平坦场景 PSR 拒绝与不达标峰高一律不采信、
  弱带阈值边界翻转、小视图回退单偏移、新模板成为最优（`DOD-04` 负向）、
  负模板零读取、E2 基线簿记与字节记账（+32/覆盖中性/terminate、track
  淘汰与 reset 释放/放不下显式 kBudgetExceeded 且池与通道不变）、容差
  含边界比较、双通道独立性、错误模型矩阵与非正预算拒绝、冻结工作量
  公式边界（55432 字节：−1 拒/等于过）、校验与预算先于取消、扫描中途
  取消不返回半份结果、双实例与重复调用逐位确定、DOD-03 坐标矩阵
  （0/90/180/270 × 奇数 21x15 × 非连续 stride）、stride 与 gray/RGBA
  格式不变性、纯 const 决策不改池。
- 门禁（文档同步时点于分支 head 复验）：debug 预设构建零告警、debug
  ctest 47/47、`mirador.fusion.object_tracker` 72 用例与验证套件 30 用例
  直跑通过；asan/ubsan 预设直跑验证套件各 30/30 且 sanitizer 零报告；
  clang-format --dry-run --Werror 对 hpp/cpp/测试文件归零；clang-tidy
  `--warnings-as-errors='*'` 对 `object_tracker.cpp` 退出码 0。
- 限制：release/tsan/warnings 预设与 asan/ubsan 全量 ctest 未在本轮执行
  ——tsan/warnings/asan/ubsan 侧已由 CI 在分支 head 覆盖（见下方 CI 回
  填；CI 矩阵无 release 预设），六预设完整门禁仍随编排脚本在分支 head
  收口（同 M7-03/M7-04 先例）；阈值/预算初值无真实先验（`DEC-019` 第 5
  条，M7-09 校准，`DOD-05` 不宣称真实场景效果）；`kUncertain`/`kLost`
  态不可经公共 API 构造，其验证路径随 M7-06 状态机落地补构造级测试；
  E2 描述量端到端联测随 M7-09 harness。
- CI 回填：PR #25 单轮 run 全绿，14/14 job——msvc/ninja、ndk/arm64-v8a、
  gcc10（focal 容器）、gcc debug/asan/ubsan/tsan/warnings 五预设、clang
  debug/fuzz、integrations-ncnn、capture/opencv 适配与 clang-format/
  clang-tidy 双口径。[run 35821784682](https://github.com/Linductor-alkaid/mirador/actions/runs/35821784682)
  （head 89f210c，覆盖实现、验证套件、出处/精度注释处置与文档回填
  commit，39m0s）；首轮通过，无修复往返。

2026-09-23：`M7-06` 证据融合与状态机实现交付（分支
`feat/m7-06-evidence-fusion-state-machine`，自 master 5653040 切出；实现于
主循环，测试按分工由 Independent-Verification-Agent 独立编写与执行，工作项
勾选随验证套件与门禁证据落地后回填）：

- 交付：`ObjectTracker::commit_track_evidence`（Experimental，帧级管线唯一
  的状态变更证据入口——M7-03 门控与 M7-05 验证器保持纯决策，分级→状态映射
  只发生在此，兑现两处冻结注记）与配套类型 `PositionScenario`/
  `TrackPositionEvidence`/`TrackEvidenceCommit`，新选项
  `impostor_match_threshold`（默认 0.8 开发冒烟值，M7-09 校准）与
  `kStateSlotOverheadBytes = 16` 状态簿记槽。消费调用方证据：`verify_track`
  的 `TrackVerification`（不重跑扫描，证据信任边界同全头）、调用方声明
  `TrackPositionEvidence`（三场景条件化 + 门控内声明）与候选语义、呈现视图
  （候选 patch 一次提取，负模板检查与模板采集共用）。冻结判定表（顶到底
  首中即停）：impostor 命中（候选 patch 对任一负模板 NCC ≥ 阈值，仅 E1 有
  候选时检查）或 DEC-010 语义冲突（双 label 非空且不等；无候选真空兼容）→
  `kVetoed`；强外观（E1 kStrong 或 E2 kConsistent）+ 门控准入 →
  `kConfirmed`；弱外观（E1 kWeak）+ 门控准入 → `kTentative`；其余 →
  `kPlaceholder`（含门控拒绝外观候选——位置门控是确认的必要条件，
  `DEC-019` 第 2 条）。条件化权重按设计 §3 表实现为显式输入面：静止/
  补偿后滚动全权重（后者由调用方声明已补偿，行为同权重、区别供 trace 与
  M7-09 分场景校准），代际切换清零位置先验（门控不作为确认必要条件、
  仅位置先验不再构成占位依据，模板/语义证据跨代保留）；补偿量计算与全局
  变化分类消费归 M7-07，本项不触碰 `estimate_global_shift` 结果流。四态
  转移冻结：确认级提交（kConfirmed/kTentative）自任意受理态恢复 kTracking
  ——kLost→kTracking 只定义状态机语义，重检测原语与身份复核入口归 M7-08
  （kLost 态位置先验失效、不再门控复捕获）；占位/否决提交将 kTracking/
  kUncertain 降级 kUncertain（否决 = 排除候选证据，外观通道无确认可用），
  连续不足计数（占位与否决均计，确认即清零）达 `uncertain_frame_limit`
  转 kLost（第 L 次连续不足提交即转移、第 L−1 次保持 kUncertain，边界
  两侧可观测）；kLost 粘滞（直至确认提交、调用方 `terminate` 或 M7-07
  代际耗尽）；kTerminated 显式 kInvalidArgument。限额按"连续不足提交数"计
  而非墙钟帧（tracker 无内部时钟，`RULE-03`；帧步进归 M7-07 管线）。
  三项契约裁决冻结于头注释：(1) 纯决策/状态变更边界；(2) 负模板采集策略
  ——语义冲突否决且未命中既有负模板时采集（impostor 恰在其首次被拒的
  确认尝试时采集一次，命中不重复入库）；kConfirmed 采集正模板
  （`max_templates >= 2` 时，kTentative 不写模板）；(3) kLost→kTracking
  归属（见上）。确认提交簿记：`last_bounds` 移至候选窗口（E1 有产出取
  best_offset，E2-only 确认位置不变）、`predicted_center` 恒为新 bounds
  中心（M7-07 前冻结不变量）、`confidence` 取钳制 E1 峰值 NCC（E2-only
  保留原值）、`last_verified_sequence` 收 `frame_sequence`、
  `layout_generation` 推进至池当前代际（每次提交）；占位/否决不改任何
  证据字段（被否决候选不得移动 track，位置先验非外观证据——M7-03 冻结）。
  状态簿记（连续不足计数 + kLost 进入时刻）为池侧单槽（`StateSlots`，同
  M7-05 基线槽先例并行存储，`TargetTrack` 冻结布局不动；`terminate`/
  track 淘汰/`reset` 释放，`byte_size`/`pool_budget_bytes` 计账，放不下
  显式 `kBudgetExceeded`）。提交原子：patch 提取先于任何变更，模板插入
  与槽分配统一预算检查（满集交换字节中性，同 `record_observation` 口径），
  任何失败池完全不变；校验先于取消（M7-06 冻结决策，同 M7-05 验证器、
  与 `adopt_track` M7-02 取消优先入口刻意对照）。`terminate` 头注释补记
  状态槽释放与"调用方驱动的 kLost/kTracking→kTerminated 边"定位（预算
  耗尽策略归 M7-08，`RULE-12`）。
- DEC-010 对接：`StableIdTracker::advance` 新增第 4 个默认参数
  `confirmed_associations`（`ConfirmedAssociation` = region_index +
  stable_id）——跟踪确认配对在门控阶段直接 kRetained（成本置优，绕过
  IoU/中心门控与成本排序），空关联列表下既有静态语义逐位不变（M4 冻结
  契约不破坏，`DEC-010` 第 4 节预留演进通道，设计 §6.5）；指向已不存在
  tracked region 的关联被忽略（区域回落常规门控）；region 索引越界、零
  id、重复索引/重复 id 显式 kInvalidArgument 且状态不变；"跟踪确认"判定
  权在调用方会话管线（消费 `ObjectTracker` 状态），本层只接受显式声明。
- 本地验证（交付时点）：debug 预设构建零告警；debug 全量 ctest 47/47
  （既有 `mirador.fusion.object_tracker` 72 用例、验证套件 30 用例、
  `stable_id_tracker` 19 用例、`perception_session` 26 用例直跑通过——
  池侧槽仅在状态转移后分配、advance 默认参数逐位保旧，既有记账/行为
  测试零回归）；开发冒烟自检（临时脚本，不入仓）10 组断言通过——
  kConfirmed + 正模板采集、`uncertain_frame_limit` 边界两侧（第 4 次保持
  kUncertain、第 5 次转 kLost）、kLost 复捕获、语义冲突否决 + 负模板采集
  （同 patch 复现时 impostor 命中且不重复采集）、代际切换场景（门控失效
  仍可外观确认、无外观降级）、kTerminated 拒绝、字节精确满池下槽分配
  拒绝且池完全不变、校验先于取消（证据不匹配在取消上下文下仍返回
  kInvalidArgument）、kCancelled/kTimeout 显式转化、DEC-010 直通（零 IoU
  远位移配对 retained + 校验矩阵 + 未跟踪 id 忽略回落）；clang-format
  全仓 dry-run 对四改动文件归零；clang-tidy `--warnings-as-errors='*'`
  对 `object_tracker.cpp` 与 `stable_id_tracker.cpp` 退出码 0（首轮 3
  处：impostor 循环改 `std::ranges::any_of`、gate 布尔返回化简、
  commit 方法认知复杂度 44>25/31>25 两轮拆分
  `validate_commit_inputs`/`extract_candidate_patch`/`apply_template_capture`/
  `plan_track_commit`/`planned_store_delta`/`apply_commit_stores` 辅助后
  归零，行为不变）。六预设 ctest、sanitizer、DOD-03 坐标矩阵、`DOD-04`
  负向、`DOD-06` 隐私负向与 CI 证据待验证套件落地后回填。
- 限制与衔接：M7-02/M7-03 限制段的 kUncertain/kLost 构造级补测（两态经
  本状态机可构造：`record_observation`/`evaluate_change_gate` 的非活跃态
  路径、`verify_track` 对两态的验证）随验证套件交付；`max_generation_lag`
  的"代际落后超阈值且证据枯竭 → kLost"判定与 `advance_layout_generation`
  触发（全局变化分类）归 M7-07，本项未实现；重检测预算耗尽策略（退避/
  最大尝试）归 M7-08，kLost→kTerminated 现经调用方 `terminate` 驱动；
  阈值初值（`impostor_match_threshold` 0.8）无真实先验（`DEC-019` 第 5
  条，M7-09 校准，`DOD-05` 不宣称滚动/swap 场景效果——`RISK-2026-16`/
  `RISK-2026-17` 随 A/B/C/D 矩阵门控，负模板机制不达标的回退为相似外观
  候选一律降级 kUncertain）；深度增强通道（M7-11~13）不在本项，未注入
  `TrackerBackend` 是默认态。

2026-09-23：验证员首轮发现 `M7-06` 两项问题，实现侧处置（处置前以分支 head
c5a7c82 的验证套件复现；不放宽公共契约，实现向已冻结契约文字对齐）：

- minor（契约缺口）：`StableIdTracker::advance` 的 `confirmed_associations`
  结构校验缺口——关联指向未跟踪 stable_id 时，重复 region_index 或重复
  stable_id 不再报错而被静默忽略（src/fusion/stable_id_tracker.cpp 原
  :297-299 未跟踪 id 在任何重复记账前 `continue`，原 :291 的重复索引检查
  只在先前关联已 pre-match 时触发），与设计 §6.5 及 advance 头注释 Errors
  列表（"duplicate region indexes or stable ids across associations"）的
  冻结文字冲突；由
  `StableIdConfirmedAssociationTest.DuplicateUntrackedAssociationsAreExplicitErrors`
  按契约文字断言暴露。修复：重复检测改为纯结构校验——`region_claimed`
  位图与 `claimed_ids` 线性扫描在 tracked 查找之前对每条关联执行，未跟踪
  id 的重复同样显式 `kInvalidArgument`（状态不变）；通过校验但指向未跟踪
  id 的关联维持既有"忽略回落常规门控"语义；原 `prev_taken` 重复分支被
  结构校验覆盖后移除（stable_id 重复已在查找前报错），错误消息文字保持
  原冻结措辞。头注释同步补记"结构校验先于未跟踪回落"的精确语义（同
  M7-05 注释精度处置先例）。处置前复现：命名用例 FAILED；修复后该用例及
  `StableIdConfirmedAssociationTest` 全部 8 用例、
  `mirador.fusion.object_tracker_evidence_fusion` 47 用例通过。
- low（注释措辞）：`commit_track_evidence` 头注释写明取消"polled at the
  entry after validation and again before the patch extraction"（两次
  轮询），实现为校验后单次轮询即进入 patch 提取（当前两者间无任何工作，
  行为不可区分）。裁决为注释向代码对齐：改为"polled exactly once, at the
  entry — after validation and immediately before the patch extraction,
  the commit's only pixel work"，不补死代码二次轮询；冻结的"校验先于
  取消"决策与单次轮询语义均不变。
- 复验（实现侧本地证据）：debug 预设构建零告警；debug 全量 ctest 48/48
  （含验证套件 `mirador.fusion.object_tracker_evidence_fusion` 47 用例、
  `stable_id_tracker` 19、`object_tracker` 72、验证器套件 30、
  `perception_session` 26 直跑全通过）；clang-format 对三改动文件归零；
  clang-tidy `--warnings-as-errors='*'` 对 `stable_id_tracker.cpp` 退出码
  0。六预设门禁与 CI 证据随编排脚本在分支 head 收口回填。

2026-09-23：`M7-06` 测试与门禁证据落地，工作项勾选（分支
`feat/m7-06-evidence-fusion-state-machine`；验证套件由
Independent-Verification-Agent 独立编写与执行，实现交付与验证员首轮两项
发现的处置见上两段）：

- 交付摘要：`ObjectTracker::commit_track_evidence` 证据融合与状态机（冻结
  判定表/四态转移/采集策略/三场景条件化输入面见本日交付段）与
  `StableIdTracker::advance` 的 `confirmed_associations` 门控直通；验证
  套件 47 用例 `tests/fusion/object_tracker_evidence_fusion_test.cpp` 随
  test(fusion) commit c5a7c82 落地。验证员首轮两项处置：340f03e 将
  advance 的重复检测升级为纯结构校验前置（`region_claimed` 位图 +
  `claimed_ids` 线性扫描先于 tracked 查找，未跟踪 id 的重复同样显式
  `kInvalidArgument`，被覆盖的不可达 `prev_taken` 分支移除，头注释冻结
  "结构校验先于未跟踪回落"——实现向已冻结契约文字对齐，契约零改动）；
  01fbc66 将 `commit_track_evidence` 取消注释向实现对齐（校验后单次入口
  轮询，不补死代码二次轮询）。本轮零测试改动（`git diff c5a7c82..HEAD --
  tests/` 为空），首轮缺陷探针按冻结契约文字断言、修复后原样通过。
- 验证覆盖：`mirador.fusion.object_tracker_evidence_fusion` 47 用例——
  `ObjectTrackerEvidenceFusionTest` 39 例（四级分级判定表 12、三场景
  条件化 2、四态转移 9、`RULE-06` 簿记 7、`DOD-04` 失效负向 2、取消/
  错误路径 2、逐位确定性 1、`DOD-03` 坐标矩阵 2、M7-02/03 两态构造级
  补测 3）与 `StableIdConfirmedAssociationTest` 8 例（DEC-010 直通/未
  跟踪回落/校验矩阵与静态语义逐位不变，含首轮缺陷探针
  `DuplicateUntrackedAssociationsAreExplicitErrors`）。
- 门禁（文档同步时点于分支 head dc21c35 复验）：六预设 ctest 全绿——
  debug 48/48（debug 多 1 条为 `mirador.adapters.opencv` 既有条目差异）、
  release/asan/ubsan/tsan/warnings 各 47/47；五个 fusion 套件直跑——
  `mirador.fusion.object_tracker_evidence_fusion` 47/47、
  `stable_id_tracker` 19/19、`object_tracker` 72/72、验证器套件
  `object_tracker_verification` 30/30、`perception_session` 26/26；
  evidence_fusion 套件 asan/ubsan 直跑 47/47 且 sanitizer 零报告，tsan
  经 `setarch -R` 直跑 47/47 零报告（均含修复后的关联校验新路径）；
  clang-format `--dry-run --Werror` 三改动文件归零；clang-tidy
  `--warnings-as-errors='*'`（`-p build/debug`）对 `stable_id_tracker.cpp`
  退出码 0。
- 限制：CI 推送与 14/14 证据回填已随 PR #26 在分支 head 收口（见下方
  CI 回填）；结构校验的 `claimed_ids` 为关联数线性扫描（最坏
  O(k²)，k 为调用方传入关联数），与既有 per-association 查找同阶，关联数
  无显式上限选项——当前调用方为会话管线自产配对（量级为 track 数），如
  M7-07 会话管线接入后出现大规模关联再议上限；`max_generation_lag` 耗尽
  判定与代际触发归 M7-07、重检测原语归 M7-08（见交付段限制与衔接）；
  阈值初值无真实先验随 M7-09 校准。
- CI 回填：PR #26 单轮 run 全绿，14/14 job——msvc/ninja、ndk/arm64-v8a、
  gcc10（focal 容器）、gcc debug/asan/ubsan/tsan/warnings 五预设、clang
  debug/fuzz、integrations-ncnn、capture/opencv 适配与 clang-format/
  clang-tidy 双口径。[run 35842711760](https://github.com/Linductor-alkaid/mirador/actions/runs/35842711760)
  （head 4c905d2，覆盖实现、验证套件、验证员首轮两项处置与文档回填
  commit，33m2s）；首轮通过，无修复往返。

2026-09-24：`M7-07` 全局运动补偿与布局代际集成实现交付（分支
`feat/m7-07-global-motion-compensation`，自 master 1ace6c6 切出；实现于
主循环，测试按分工由 Independent-Verification-Agent 独立编写与执行，工作项
勾选随验证套件与门禁证据落地后回填）：

- 交付：`ObjectTracker` 三个池侧原语（Experimental，设计 §6.3/§6.4；
  M7-04/M7-06 显式预留消费侧收口；管线编排形态冻结为池侧原语 + 调用方
  组合帧管线，M7-03/05/06 先例，编排不进本层，`PerceptionSession` 仍不持有
  tracker）。(1) `advance_generation_for_classification`：冻结触发判定——
  `kGlobal` → 代际 +1（`GenerationAdvance{advanced, generation}` 回显），
  `kNone`/`kPartial` 不动，未知枚举值显式 `kInvalidArgument`；uint32 耗尽
  经 `advance_layout_generation` 透传 `kBudgetExceeded`（显式失败口径维持）；
  代际切换后逐 track 降级（kTracking 无确认证据 → kUncertain、位置先验
  清零）维持 M7-06 状态机语义——经调用方以 `PositionScenario::
  kGenerationSwitch` 场景提交驱动，代际递增本身不改写 track 状态（M7-06
  冻结不动）。(2) `compensate_global_motion`：消费调用方
  `estimate_global_shift` 结果（证据信任边界同全头，tracker 不自持上一帧、
  不自跑原语），对全部非 `kTerminated` track 施加同一位移校正
  （`last_bounds`/`predicted_center` 逐分量 float 平移 + 中心按既冻结公式
  重算；kTerminated 归档留在终止位置；位置历史/模板/负模板/E2 基线/代际/
  状态槽不动——补偿是坐标更新不是证据，不改写历史观测）；结果
  `MotionCompensationResult`（`applied`/`dx`/`dy`/逐 track 升序 echo
  `MotionCompensationEntry`，≤ `max_targets` 有界）；置信度门
  `min_compensation_confidence`（新选项，[0,1]，默认 0.0 不过滤——开发
  冒烟值，M7-09 校准，`RISK-2026-17` 补偿失效门控旋钮）不达标显式
  `applied=false` 拒绝、池不动（RULE-06 不静默丢弃）；非有限 dx/dy、
  置信度越界（含 NaN）、平移越 float 有限域均显式 `kInvalidArgument` 且池
  完全不变（先验证全体后变异，原子）。`predicted_center` 恒为
  `last_bounds` 中心的冻结不变量**维持**（取舍冻结于头注释：M7-01 布局无
  速度字段，设计 §6.3 "更新速度估计"以逐帧平移本身实现，速度模型留 M7-09
  在 Experimental 内提议；设计 §6.3 落点注记已同步）。补偿后调用方以
  `PositionScenario::kCompensatedScroll` 声明场景（行为同 kStationary 全
  权重，区别供 trace 与 M7-09 分场景校准——M7-06 冻结不动）。(3)
  `sweep_generation_lag`：`max_generation_lag` 耗尽判定落地（M7-06 明确
  归本项），"证据枯竭"冻结为双条件——track 代际落后池当前代际**大于**
  `max_generation_lag`（每次提交含占位与否决都把 track 代际戳到池当前值，
  "落后"等价于连续超限个代际未收到任何等级提交）**且** track 处于
  kUncertain（M7-06 已判定证据不足、落后窗口内无确认证据到达）→ kLost 并
  经状态槽记录丢失时刻（防御性分配 + 预算检查；kUncertain 恒已持槽），
  逐 id 升序 trace 显式上报（RULE-06 降级不静默）；kLost 粘滞语义不变
  （kLost→kTracking 仍只经确认提交或 M7-08 身份复核）；kTracking 保持
  确认态——其降级路径是 M7-06 提交链（kGenerationSwitch 场景提交），清扫
  不伪造证据不足。先规划后变异，任何失败池完全不变。三原语取消/超时均为
  入口单次轮询（`commit_track_evidence` 冻结先例——全池一遍每 track 常数
  量有界工作、验证后变异无失败路径，中途轮询只会打断半应用的池）。头注释
  同步兑现全部 M7-06 预留钩子注记（`PositionScenario`、
  `advance_layout_generation`、`evaluate_change_gate` kGlobal 注记、
  `commit_track_evidence` kLost 粘滞与 predicted_center 注记、
  `layout_generation()` getter、`TargetTrack::predicted_center`）。新增
  头依赖仅 `<mirador/shift_estimation.hpp>`（`ShiftEstimate` 消费），
  `mirador_fusion` 链接接口恰为 core/image/cache 不变（shift_estimation
  属 image 模块，架构测试与链接闭包探针不受影响）。本项未引入任何关联
  （`confirmed_associations`）构造路径，M7-06 限制段的关联数上限议题不
  触发。
- 本地验证（交付时点）：debug 预设构建零告警；debug 全量 ctest 48/48
  （既有五个 fusion 套件含 `object_tracker` 72、`object_tracker_verification`
  30、`object_tracker_evidence_fusion` 47 用例零回归）；开发冒烟自检
  （临时脚本 `/tmp`，不入仓）九组断言全过——触发判定三分类 + 未知枚举 +
  uint32 语义、滚动往返（合成纹理帧对 `estimate_global_shift` 得
  (dx,dy)=(8,4)、adopt → compensate → `verify_track` 于滚动后帧 kStrong
  峰值 NCC 1.0、偏移 (0,0)、PSR 5.27——补偿后位置先验恢复门控有效性的
  往返闭环）、中心不变量与历史不改写、kTerminated 排除、置信度门显式
  拒绝池不动、NaN/非有限/float 溢出显式 `kInvalidArgument`（两步越界构造
  验证原子性）、取消上下文 kCancelled、耗尽清扫（lag=1 边界不扫、lag=2
  扫出且 kTracking 不动、kLost 粘滞、二次清扫空）、kGenerationSwitch
  级联（kTracking → kUncertain + 代际戳记）、双实例逐位确定性；
  clang-format 全仓 dry-run 对两改动文件归零（首轮 3 处违规已修）；
  clang-tidy `--warnings-as-errors='*'`（`-p build/debug`）对
  `object_tracker.cpp` 退出码 0（首轮 3 处：include-cleaner 直接包含
  `shift_estimation.hpp` 与 modernize-use-auto 两处 cast 自动类型后归零，
  行为不变）。六预设 ctest、sanitizer、DOD-03 坐标矩阵、DOD-04 负向与 CI
  证据待验证套件落地后回填。
- 限制与衔接：补偿不产生速度状态、置信度门与耗尽判定阈值均为开发冒烟
  初值（`DEC-019` 第 5 条，M7-09 校准，`DOD-05` 只宣称合成口径）；
  `RISK-2026-17` 门控随 M7-09 A/B/C/D 矩阵 B/C 差值与 `*-scroll` 延续率
  收口，回退为收紧代际判定或位置通道降权；重检测原语与身份复核归 M7-08
  （kLost→kTerminated 预算策略不动）；基准与门槛校准归 M7-09；
  `StableIdTracker::advance` 冻结契约零改动（DEC-010 直通通道 M7-06 已
  落地，本项未触碰）；DOD-03 方向/奇数尺寸/非连续 stride/贴边往返矩阵、
  DOD-04 模板失效负向与隐私负向由 Independent-Verification-Agent 验证
  套件覆盖后回填本记录。

2026-09-24：`M7-07` 测试与门禁证据落地，工作项勾选（分支
`feat/m7-07-global-motion-compensation`；验证套件由
Independent-Verification-Agent 独立编写与执行，实现交付见上段）：

- 交付摘要：三个池侧原语（冻结触发判定/池级位移校正/`max_generation_lag`
  耗尽清扫，语义冻结见本日实现交付段）的验证套件 19 用例
  `tests/fusion/object_tracker_motion_generation_test.cpp` 随 test(fusion)
  commit e231d12 落地（注册 ctest 项
  `mirador.fusion.object_tracker_motion_generation`，LABELS unit）。验证轮
  零实现改动：实现审查对照冻结契约逐条核对未发现缺陷，非缺陷事实经测试
  钉住——compensate/sweep 为「入口先轮询取消、后校验」顺序（与
  `commit_track_evidence` 冻结的校验先于取消刻意不同，与其自身头注释
  一致，`CompensationCancellationAndTimeout` 钉住）；置信门为闭区间
  （confidence == 阈值应予应用）。M7-06 交付段移交的五项预留义务全部有
  构造性测试覆盖：触发判定（义务1）、kGenerationSwitch 降级链（义务2）、
  耗尽清扫（义务3）、补偿消费（义务4）与滚动往返（义务5）。
- 验证覆盖：`mirador.fusion.object_tracker_motion_generation` 19 用例——
  触发判定 4 例（选项域校验、kGlobal 恰 +1 与 `evaluate_change_gate`
  const 纯决策不递增（M7-03 延迟注记兑现验证）、未知枚举显式
  kInvalidArgument 池不变、代际递增不改任何 track 字段与字节账）；补偿
  8 例（全池逐字段精确平移 + kTerminated 归档不动 + 中心不变量 + 历史/
  模板/代际/状态/字节账不动；置信门闭区间显式 `applied=false` 拒绝与
  默认 0.0 不过滤；NaN/越界置信度/float 溢出显式失败与双轨兄弟原子性
  （先规划后变异）；入口取消/超时冻结顺序；空池/零位移/同一估计重复喂入
  的管线纪律；双实例逐位确定性）；耗尽清扫 5 例（严格大于边界两侧、
  kUncertain 单条件 + kLost 粘滞不重报 + kTracking 保持确认态、清扫后
  kLost 占位不复活直至确认提交复捕获并戳当前代际、取消/超时显式转化、
  空清扫显式空集）；kGenerationSwitch 级联降级与恢复 1 例；估计器在环
  滚动往返 1 例——补偿前同一 kPartial 报告 kReuse 短路且验证 kNone
  （8px 轨道滚动 (8,4) 超出冻结验证 ROI 扩展半径 ≈ 半对角 5.7px/侧，
  验证器完全够不到目标——`RISK-2026-17` 结构性失效与补偿存在理由的
  实证，供 M7-09 校准参考）、补偿后同一报告 kVerify + 同帧
  kStrong@offset(0,0) 峰值 NCC > 0.999 + `kCompensatedScroll` 提交
  kConfirmed、bounds 恰落真位；DOD-03 坐标矩阵 1 例（0/90/180/270 旋转
  元数据 × 奇数 21×15 × +7 非连续 stride × 贴边 × 正负两方向：平移逐位
  与元数据无关、往返 kStrong@(0,0) 成立）。隐私（`RULE-10`/`DOD-06`）
  按 M7-03/05/06 套件先例结构性交底：结果类型只携带 id/枚举/状态/坐标
  矩形，无模板字节、缩略图或帧内容可泄，tracker 无日志/文件系统/网络
  面；fusion 管线二进制由共享隐私套件覆盖（`tests/privacy`，M5-07 注册
  口径）。DOD-04 模板失效负向无 M7-07 专属面——补偿只动坐标不触碰模板/
  基线，失效翻转已由 M7-05/M7-06 验证套件在验证/提交缝覆盖
  （`object_tracker_verification_test.cpp`、
  `object_tracker_evidence_fusion_test.cpp`）。
- 门禁（文档同步时点于分支 head e231d12 复验）：debug 预设全量 ctest
  49/49（7 项架构/链接闭包测试随全量通过，`mirador_fusion` 链接接口恰
  为 core/image/cache 不变，新增 `shift_estimation.hpp` 头依赖仍在既有
  链接面内）；新套件 debug 直跑 19/19，asan/ubsan 预设直跑各 19/19 且
  sanitizer 零报告；clang-format `--dry-run --Werror` 对三改动文件
  （hpp/cpp/测试）归零；clang-tidy `--warnings-as-errors='*'`
  （`-p build/debug`）对 `object_tracker.cpp` 与新测试文件退出码 0。
  首轮 lint 往返（测试文件 10 处 format 违规与 8 处 tidy 告警：未用
  using、补 `<mirador/pixel_format.hpp>` 与 `<optional>`、
  EnumCastOutOfRange NOLINT（tests/core 先例）、认知复杂度 NOLINT、
  optional 未检查访问改守卫式解引用）已随 e231d12 修复，用例语义不变。
- 限制：uint32 代际耗尽的 `kBudgetExceeded` 透传仅经代码审查验证
  （`src/fusion/object_tracker.cpp:1073` 守卫、`:1270` 透传；公共 API
  需 2^32 次调用，单测不可行，同 M7-02 冻结套件先例）；sweep 防御性状态
  槽分配的 `kBudgetExceeded` 分支经公共 API 不可达（kUncertain 轨迹必已
  持降级提交分配的槽），套件改为验证可达行为（清扫后 `byte_size` 不
  变）；release/tsan/warnings 预设与六预设完整复跑随编排脚本收口——
  tsan/warnings 侧已由 CI 在分支 head 覆盖（见下方 CI 回填；CI 矩阵无
  release 预设，同 M7-03~06 先例）；滚动往返为合成口径（`DOD-05`
  不宣称真实场景效果），`min_compensation_confidence` 0.0 与
  `max_generation_lag` 1 为开发冒烟初值（`DEC-019` 第 5 条，M7-09 校准
  收口，`RISK-2026-17` 随 A/B/C/D 矩阵 B/C 差值与 `*-scroll` 延续率门
  控）；重检测原语与身份复核归 M7-08；既有遗留（非本项引入）：头文件
  声明的单参 `ObjectTracker::terminate(uint64_t)` 重载在 src 中无定义
  （object_tracker.hpp:594，冒烟曾触发链接错误），建议 M7-08 或后续
  refactor 清理。
- CI 回填：PR #27 单轮 run 全绿，14/14 job——msvc/ninja、ndk/arm64-v8a、
  gcc10（focal 容器）、gcc debug/asan/ubsan/tsan/warnings 五预设、clang
  debug/fuzz、integrations-ncnn、capture/opencv 适配与 clang-format/
  clang-tidy 双口径。[run 35896974845](https://github.com/Linductor-alkaid/mirador/actions/runs/35896974845)
  （head c52a200，覆盖实现、契约注册、验证套件与文档交付/勾选 commit，
  38m1s）；首轮通过，无修复往返。

2026-09-24：`M7-08` 实现交付（分支 `feat/m7-08-cascade-redetection-identity-review`
自 master 3b517a9 切出；测试由 Independent-Verification-Agent 独立编写与
执行，随验证套件落地后另行勾选）：

- 契约冻结（object_tracker.hpp）：级联重检测四原语——`evaluate_redetection_gate`
  （纯 `const` 退避门查询：kNone 静止画面一律 kHoldStaticFrame 零触发、
  退避窗口内 kHoldBackoff、其余 kTrigger、非 kLost 显式 kInactive；变化
  ROI 刻意不消费——kLost 位置先验失效不得门控复捕获，同 M7-06 冻结）、
  `record_redetection_failure`（失败尝试记账 + 冻结倍增退避
  `min(base × 2^(n-1), max)` 帧序列计 + 连续失败达 `redetect_max_attempts`
  由该入口自身执行 kLost → kTerminated 归档转移）、
  `record_redetection_recapture`（复捕获确认后有界中断事件写入 +
  回合槽关闭；要求 track 已 kTracking——确认提交先行）与
  `record_redetection_association`（新 ID 分支身份交接关联，前置 kLost
  前驱；诊断性记录不改状态）。const/状态变更边界、回合槽陈旧键（回合
  kLost 进入时刻）、有界日志表示（`RedetectionRecord` 仅 id 与序列，
  `RULE-10`）、`max_redetection_records` 选项与
  `kRedetectSlotOverheadBytes`/`kRedetectionRecordOverheadBytes` 记账
  冻结于头注释；`TargetTrack` 布局不动。身份复核本体零新验证代码：复用
  `verify_track`（M7-05）+ `commit_track_evidence`（M7-06 kLost →
  kTracking 复捕获语义不重写）；新 ID 分支走常规融合采纳（`adopt_track`，
  `DEC-010`）。随项移除 M7-07 验证记录指出的无定义单参
  `terminate(uint64_t)` 死声明（独立 refactor commit，先于实现落地）。
- 实现要点（src/fusion/object_tracker.cpp）：`terminate` 归档语义重构为
  共享 `archive_track` 助手（调用方驱动边与预算耗尽边同一语义，行为
  不变）；入口单次取消轮询（M7-07 先例，O(1) 入口校验后无失败路径者
  cancel-first——门查询同 `evaluate_change_gate`，记账同
  `compensate_global_motion`）；回合槽/日志插入、淘汰与字节记账沿用
  `record_observation`/M7-05/06 槽位先例；淘汰计划计入回合槽字节
  （`plan_eviction`）；`reset` 清理全部新状态。设计 §7 第 2 条"按语义
  标签过滤候选的通用组件"不在本工作项文本内，落点注记显式声明随后续
  工作项或上层集成交付。
- 本地验证（实现交付时点）：debug 构建零告警；全量 ctest 49/49（既有
  4 个 object_tracker 套件零回归；首次全量出现 1 例未复现失败，未捕获
  用例名，连续 4 轮全量复跑全绿——按抖动处理，若验证轮再现按
  Independent-Verification-Agent 流程上报）；开发冒烟 118 断言通过
  （门判定矩阵含静止画面零触发、倍增序列 1→2→4 与封顶、预算耗尽
  kTerminated 归档、回合陈旧键重置、中断事件/关联校验矩阵、有界日志
  溢出淘汰计数、字节记账逐项、双实例逐位确定性、取消/超时显式转化）；
  clang-format/clang-tidy 双口径在两份实现文件归零（CI 口径复跑随编排
  脚本）。PSR 平坦门冒烟注记：默认 `peak_sidelobe_ratio_min` 5.0 下
  同帧精确匹配峰值 PSR≈4.6 被拒（平坦门按设计工作），冒烟以 2.0 验证
  复核通路——阈值初值随 M7-09 校准（`DEC-019` 第 5 条）。
- 移交验证员：静止画面零触发负向（计划退出条件、设计 §8 强制）、退避
  序列符合配置（1→2→…→60 封顶，确定性、无墙钟）、预算耗尽显式失败
  （kTerminated 可见、错误路径池不变）、复核通过延续/新分配两分支、
  中断事件与关联记录的有界性（RULE-06 负向）、取消/超时转化、字节
  记账与回合槽生命周期（terminate/淘汰/reset 释放）、DOD-03/04/06
  负向、双实例逐位确定性；六预设门禁与 CI 证据随验证套件落地回填。

2026-09-24：`M7-08` 验证轮处置（验证套件
`tests/fusion/object_tracker_redetection_test.cpp` 随 test(fusion)
commit 落地；实现侧修复由验证员 scratch 复核发现的一处低severity缺陷）：

- 缺陷与修复：验证员 scratch 复核证实 `record_redetection_recapture`
  读取中断事件 `attempts` 时未施加回合视图陈旧键校验（与
  `evaluate_redetection_gate`/`record_redetection_failure` 的键校验路径
  不一致）——track 于 seq2 丢失、记账失败 1 次、经 `commit_track_evidence`
  走入式复捕获（无簿记）、再丢失于 seq8 后，回合 2 的中断事件错误携带
  已死回合 1 的计数（冻结语义应报 0：回合槽陈旧即按新回合读取，头注释
  §M7-08 节冻结规则）。影响限于诊断记录字段失真——退避调度、门判定与
  耗尽转移均走键校验路径，行为不受影响。修复：recapture 读取处以调用方
  `lost_sequence` 证据为回合键施加同一陈旧规则（确认提交已清零状态槽
  kLost 进入时刻，调用方证据是此处唯一可得键，与既有证据信任边界一致，
  不放宽公共契约）；头注释同步精确化该键语义。验证员附注口径维持：两
  回合 kLost 进入序列相同（调用方帧序列不前进）时陈旧性按设计不可分辨，
  不另立缺陷。
- 修复回归（本会话执行）：开发冒烟追加验证员复现场景（死回合计数不
  泄漏——stale 事件 attempts==0；合法回合仍精确上报——键匹配事件
  attempts==2）共 137 断言全过；全量 debug ctest 50/50（含验证套件
  `mirador.fusion.object_tracker_redetection`，既有用例零改动通过——
  套件 recapture 调用均以匹配键调用，修复不改变其断言路径）；
  clang-format/clang-tidy 双口径在两份实现文件归零。六预设与 CI 复跑
  随编排脚本收口。

2026-09-24：`M7-08` 测试与门禁证据落地，工作项勾选（分支
`feat/m7-08-cascade-redetection-identity-review`；验证套件由
Independent-Verification-Agent 独立编写与执行，实现交付与验证轮缺陷处置
见上两段）：

- 交付摘要：级联重检测四原语（冻结语义见本日实现交付段）的验证套件
  22 用例 `tests/fusion/object_tracker_redetection_test.cpp` 随
  test(fusion) commit d8c439d 落地（注册 ctest 项
  `mirador.fusion.object_tracker_redetection`，LABELS unit）；验证员
  scratch 复核发现 `record_redetection_recapture` 的 attempts 读取缺
  回合陈旧键校验（低严重度、诊断字段失真，见上段处置），实现侧修复
  0a95f79——attempts 读取施加回合视图陈旧键规则、以调用方
  `lost_sequence` 证据为回合键（`slot->second.episode_lost_sequence ==
  lost_sequence` 才读槽内计数，否则按新回合报 0，
  src/fusion/object_tracker.cpp:1628-1637；确认提交已清零状态槽 kLost
  进入时刻，调用方证据是 recapture 时点唯一可得回合键，与全头证据信任
  边界一致），头注释同步冻结该键语义（object_tracker.hpp:1408-1415，
  纯注释澄清已冻结语义，无签名/行为面放宽）；修复回归
  `RecaptureAttemptsApplyTheCallerEpisodeKeyRule` 随 test(fusion)
  commit a578628 落地（22 → 23 用例；既有 22 例零改动通过——套件
  recapture 调用均以匹配键调用，无测试过时）。
- 验证覆盖：`mirador.fusion.object_tracker_redetection` 23 用例——
  门判定 3 例（静止画面 `kNone` 一律 kHoldStaticFrame 零触发——计划
  退出条件与设计 §8 强制负向、判定矩阵显式、未知/终止/非法分类拒绝）；
  退避 2 例（冻结倍增公式 `min(base × 2^(n-1), max)` 逐帧计、自定义
  base 与封顶以帧序列计无墙钟）；预算耗尽 3 例（连续失败达
  `redetect_max_attempts` 由记账入口自身执行 kTerminated 归档显式可见、
  单次预算首败即终、回合槽/日志放不下显式 `kBudgetExceeded` 且池与
  回合状态不变）；复捕获 5 例（入口校验矩阵、回合关闭与再丢失新回合、
  无簿记走入式陈旧槽读作新回合、修复回归三分支——死回合计数不泄漏
  attempts==0/键匹配精确上报/错键读作新回合且陈旧槽随回合关闭释放、
  字节足迹等于无失败双实例 + 恰好一条日志记录）；关联记录 1 例（纯
  诊断不改状态、全量校验）；有界日志 2 例（溢出淘汰最旧并显式计数、
  `max_redetection_records` 有界）；回合槽生命周期与字节记账 1 例
  （terminate/淘汰/reset 释放）；身份复核两分支 2 例（复核通过经冻结
  M7-05 `verify_track` + M7-06 `commit_track_evidence` 延续 ID + 中断
  事件；证据不足经 `adopt_track` 新 ID + 关联记录）；取消/超时全入口
  显式转化、双实例逐位确定性、隐私（`RedetectionRecord` 仅 id 与序列，
  `RULE-10`）与 DOD-03 坐标矩阵（0/90/180/270 旋转 × 奇数 21×15 ×
  +7 非连续 stride 下复核复捕获链路成立）各 1 例。
- 门禁（文档同步时点于分支 head a578628 复验）：`cmake --build
  --preset debug` 增量 up-to-date（干净构建零告警为修复会话证据）；debug
  全量 ctest 50/50（label 汇总 architecture 7 / property 1 / unit 42，
  既有 fusion 套件零回归）；新套件直跑 debug 23/23、asan/ubsan 预设直
  跑各 23/23 且 sanitizer 零报告；clang-format `--dry-run --Werror` 对
  三改动文件（hpp/cpp/测试）归零；clang-tidy
  `--warnings-as-errors='*'`（`-p build/debug`）对 `object_tracker.cpp`
  与新测试文件退出码 0。修复会话缺陷双向证实（/tmp scratch 复现程序，
  不入仓）：修复前同场景 attempts==1（即上轮报告的缺陷），修复后
  'A: stale episode event attempts=0' 与 'B: keyed episode event
  attempts=1' 双向断言通过、开发冒烟累计 137 断言全过——检查具区分
  力且未伤键内路径。
- 限制：调用方提供的 `lost_sequence` 与当前回合真实键不符（证据错误）
  时 attempts 读 0 且陈旧/错键槽仍随回合关闭释放——诊断字段失真由调用
  方证据负责，与全头证据信任边界一致（头注释冻结）；两回合 kLost 进入
  序列相同（调用方帧序列不前进）时陈旧性按设计不可分辨（验证员附注
  口径维持，不另立缺陷）；修复仅触诊断字段——退避调度、门判定与耗尽
  转移走既有键校验路径，行为零变化；release/tsan/warnings 预设与六预设
  完整复跑随编排脚本收口——tsan/warnings 侧已由 CI 在分支 head 覆盖
  （见下方 CI 回填；CI 矩阵无 release 预设，同 M7-03~07 先例）；
  退避/预算初值为开发冒烟值，M7-09 校准（`DEC-019` 第 5 条）。
- CI 回填：PR #28 单轮 run 全绿，14/14 job——msvc/ninja、ndk/arm64-v8a、
  gcc10（focal 容器）、gcc debug/asan/ubsan/tsan/warnings 五预设、clang
  debug/fuzz、integrations-ncnn、capture/opencv 适配与 clang-format/
  clang-tidy 双口径。[run 35917953109](https://github.com/Linductor-alkaid/mirador/actions/runs/35917953109)
  （head 86c6433，覆盖实现、契约注册、验证套件与文档交付/勾选 commit，
  37m18s）；首轮通过，无修复往返。

2026-09-24：`M7-09` 合成验证 harness 与基准发布交付，工作项勾选（分支
`feat/m7-09-synthetic-harness-benchmarks` 自 master e9d9d0c 切出；harness
实现于本工作项，测试与门禁由 Independent-Verification-Agent 按分工另行
覆盖——既有 fusion 套件已含确定性/预算负向，harness 按 benchmarks 既有
先例纳入编译与 lint 验证，六预设 ctest 与 CI 证据随编排脚本收口）：

- 交付：`benchmarks/object_tracking_bench.cpp`（`mirador_bench_object_tracking`
  注册于 benchmarks/CMakeLists.txt，链接 `mirador::fusion`，零新依赖）。harness
  为调用方组合帧管线（M7-03~08 冻结形态）：每帧 `detect_change`(M1) →
  `StableIdTracker::advance`（DEC-010，D 方法传 confirmed_associations 直通）→
  `advance_generation_for_classification`（M7-07）→（kPartial）
  `estimate_global_shift`(M7-04)+`compensate_global_motion`（C/D）→
  `evaluate_change_gate`（M7-03）→ kLost 重检测（`evaluate_redetection_gate`
  + oracle Detector 粗召回（批调用计数，RULE-12 触发策略在 harness）+
  `verify_track` 身份复核 + `commit_track_evidence` + `record_redetection_*`）
  → 活跃 track `verification_roi` + E2 结构测度 + `verify_track`(M7-05) +
  `commit_track_evidence`(M7-06) → 采纳 + `record_structure_baseline` →
  GT 对账 → `sweep_generation_lag`。A/B/C/D 为调用方策略差（A 无短路每帧
  全验证/门控透明；B 门控无补偿；C 加补偿；D 加候选语义与融合直通），消费
  的全部为已冻结公共契约，`object_tracker.hpp` 语义零修改。
  六场景（`linux-static-page`/`-scroll`/`-dialog`/`-theme-switch`/
  `-similar-icons`/`-partial-anim`，64×32 对象置于块对齐卡片使 M1 块差分
  可见亚块运动）：滚动步长 48 px 刻意超出冻结验证 ROI 半径 ±36 px（B/C
  分离的结构来源）；dialog 为模态面板 + 全帧棋盘格环境（kGlobal + 静态
  开启期零触发负向）；similar-icons 为孪生纹理 + 亮色弹出面板（交叉 NCC
  0.712 落弱带、弹出物 +24 px 在 ROI 内——swap 机制）；theme-switch 全帧
  luma 反转（E1 反相失效、E2 边缘测度不变）；partial-anim 动画窗覆盖
  4 帧（占位 → kUncertain → 恢复）。
- 基准发布：[linux-x64-object-tracking-2026-09](../benchmarks/linux-x64-object-tracking-2026-09.md)
  （`DEC-011` 口径：Linux x64 release、3 次重复非计时指标逐位一致、场景
  清单与合成口径限定、同日 M1 基线对照、复现命令）。§8 八项指标 × 24 cell
  全套数字；`DEC-019` 第 5 条门槛逐项判定：static-page 延续 1.000 ≥ 0.95 ✓、
  scroll 补偿后 1.000 ≥ 0.90 ✓、similar-icons swap 仅 D = 0 ≤ 0.05 ✓、
  假阳性延续仅 D = 0 ≤ 0.02 ✓、静止帧短路对 M1 同日基线无可测回归 ✓、
  kLost 后静止画面零 Detector 触发（内建断言全过）✓。`RISK-2026-17` 证据：
  B/C 差值（scroll 静止期延续 0.333 vs 1.000、Detector 120/min vs 0、
  B 四对象 new-ID 替换 vs C 零丢失）；`RISK-2026-16` 证据：A/B/C 各 2 次
  身份交换 vs D 经语义否决（5 次）+ impostor 命中（4 次）零交换（代价：
  D 该对象 2 帧误判丢后 2 帧重捕获）。
- 门槛初值逐项校准（`DEC-019` 第 5 条，库默认全部以测量依据维持，无一处
  变更）：详见报告校准表。要点：`peak_sidelobe_ratio_min` 5.0 热点——初版
  合成纹理实测真匹配 PSR 4.96 < 5.0（同 M7-08 冒烟 4.6 现象），归因于贫纹理
  刺激而非阈值，场景纹理富化后真匹配 PSR ≥ 6.95、杂峰 ≤ 3.5，维持 5.0 并
  记录真实数据复核条件；`min_compensation_confidence` 库默认 0.0 维持
  （M7-07 套件钉住），测量依据支持调用方配置 0.7——真滚动置信 [0.93, 0.95]
  vs 局部变化帧 [0.46, 0.55]（0.0 门下 C/D 曾对局部变化帧以伪位移平移整池，
  0.7 后全部显式 `applied=false` 拒绝），harness 采用 0.7 并在此记录，库默认
  是否上调留 M7-10 判定；其余逐项测量证据见报告。
- 本地验证（交付时点）：release/debug 构建零告警；release 全矩阵 3 次重复
  非计时指标逐位一致（内建确定性断言）、kLost 静止零触发断言全过、池
  `byte_size ≤ pool_budget_bytes` 逐帧断言全过（峰值 39,960 B = 1 MiB 预算
  的 3.8%）；静止帧短路：detect 单独 p50 3455.7 µs vs 完整前缀 p50 3448.2 µs、
  M1 同日复测 unchanged p50 3501.6 µs——无可测回归；clang-format 全仓 dry-run
  对改动文件归零、clang-tidy `--warnings-as-errors='*'`（-p build/release）
  对 `object_tracking_bench.cpp` 归零。debug 冒烟六场景跑通（详见下段）。
  六预设 ctest、sanitizer 与 CI 证据由 Independent-Verification-Agent 与编排
  脚本收口。

同日 debug 冒烟（debug 预设构建零告警后
`./build/debug/benchmarks/mirador_bench_object_tracking 2`，2 次重复）：
24 cell 全部跑通、场景分类自断言/零静态触发/池预算/逐位确定性断言全过、
退出码 0——debug 口径下定性结论与 release 一致（如 static 短路 detect
p50 22496.9 µs vs 完整前缀 p50 22411.8 µs，无可测回归），计时数字仅具
release 口径效力（`DEC-011`）。

2026-09-24：`M7-09` 验证轮处置（验证员复核未发现阻碍验收的缺陷，两项
实现侧处置随 fix commit be68658 落地；验证套件由验证员随 test(fusion)
commit f8926a2 落地）：

- 轻微·潜在：重捕获延迟账本只覆盖 commit 路径丢失——`cell_set_lost_sequence`
  仅由 `reconcile_commit` 调用，而 `sweep_generation_lag` 也能把 kUncertain
  track 判为 kLost 且被清扫 id 列表被丢弃；该路径丢失的 track 若日后重捕获，
  延迟会按缺失的 kLost 记录（0）计算并静默发布虚高值，中断事件也会携带
  `lost_sequence=0`。处置：`frame_step` 经新增 `sweep_step` 消费 sweep 返回
  的被清扫 id，进入与 commit 路径同一丢失账本（丢失序列 + loss_events）。
  当前矩阵无 sweep 丢失，发布数字不受影响——修复后全矩阵复跑与已发布
  3 次重复采集逐行一致（非计时差异行数 0，实测命令
  `diff` 于两份 stdout，exit 0）。
- 外观：harness 横幅声称 "frozen M7 defaults" 而 C/D 实配
  `min_compensation_confidence=0.7`（库默认 0.0 维持）——措辞改为如实
  说明库默认与调用方策略两部分；报告与验证记录原表述本已准确。
- 观察（非缺陷，无需改动）：调用方以 float 传入 confidence、门为 double
  时的表示精度边界由契约冻结（M7-07 套件已在可精确表示的 0.5 钉住闭区间），
  harness 实测置信带 [0.93, 0.95] / [0.46, 0.55] 远离 0.7 边界，已发布结论
  不受影响；验证员已在其测试注释中说明。
- 复验（本会话执行）：修复后 debug/release 构建零告警、clang-format 全仓
  dry-run 归零、clang-tidy `--warnings-as-errors='*'`（-p build/release）
  对 `object_tracking_bench.cpp` 归零；release 全矩阵（2 次重复）退出码 0、
  全部内建断言（零静态触发/池预算/逐位确定性）通过，24 cell 非计时指标与
  已发布数字逐行一致。未运行项：六预设完整 ctest 与 CI 14/14 回填
  （不推送/不合并，属编排脚本收口）。
- 限制：全部为合成口径（`DOD-05`），真实截图评估（`RISK-2026-14` /
  `DEC-018` 阶段 2 共享采集）是转正前置不属本项，M7-10 判定须显式记录
  "仅有合成证据"；E2 结构测度由 harness 测量而非真实 `propose_regions`
  输出（E2 描述量消费契约的端到端联测以此口径覆盖）；oracle Detector 按
  GT 粗召回（设计 §7"按语义标签过滤候选的通用组件"不属本项，未实现）；
  A 方法的"仅外观"在决策层隔离，E1 搜索仍受冻结 ROI 机械约束（契约无
  全帧搜索面）；swap 率量纲为"每对象交换次数"（分母随场景定义冻结）。

2026-09-24：`M7-09` 本地全量门禁收口（Independent-Verification-Agent
补跑上一条处置记录中挂起的六预设与 lint 项，验证轮 ready 未决状态就此
闭合——全部测试通过、无未处置缺陷）：

- 六预设构建 + ctest 全部退出码 0：debug 51/51（多出 1 项为 debug 预设
  独有的 OpenCV 可选模块测试 `mirador.adapters.opencv`，与本分支无关，
  经 `ctest -N` 列表比对确认）、release/warnings/asan/ubsan/tsan 各
  50/50；分支新增 `mirador.fusion.object_tracker_calibration` 在全部
  预设通过。
- lint 归零：`clang-format --dry-run --Werror` 与
  `clang-tidy --warnings-as-errors='*'`（-p build/debug）对两处变更
  C++ 文件（`benchmarks/object_tracking_bench.cpp`、
  `tests/fusion/object_tracker_calibration_test.cpp`）零告警。
- harness 复跑零漂移：release 口径 `mirador_bench_object_tracking 2`
  两次运行退出码 0、首行 `self-checks ok`（零静态触发/池预算/逐位
  确定性内建断言全过）；两次运行非计时行归一计时字段后 `diff` 为空；
  与已发布基线文档逐项对照 323 项断言零漂移（ID 延续率、swap/假阳性、
  丢失误判与重捕获、Detector 触发、池内存、融合保留率与叙事数字全量
  覆盖 24 cell）。
- CI 回填：PR #29 单轮 run 全绿，14/14 job——msvc/ninja、ndk/arm64-v8a、
  gcc10（ubuntu-20.04 容器）、gcc debug/asan/ubsan/tsan/warnings 五预设、
  clang debug/fuzz、integrations-ncnn、capture/opencv 适配与
  clang-format/clang-tidy 双口径（lint job 排队后 38m39s 完成）。
  [run 35963460642](https://github.com/Linductor-alkaid/mirador/actions/runs/35963460642)
  （head 75f8ee8，覆盖实现、校准测试、验证轮处置与门禁证据 commit，
  全程 38m43s）；首轮通过，无修复往返，上条"待办：CI 14/14 回填随 PR
  收口"就此闭合（文档表格中的 M1 同日计时对照属运行日墙钟样本，按
  `DEC-011` 口径不入零漂移判定，本轮 harness 计时行定性一致——前缀
  ≤ detect 单独，无可测回归）。

2026-09-28：`M7-10` go/no-go 判定与转正决策草案交付，工作项勾选（分支
`feat/m7-10-gonogo-verdict-promotion-draft` 自 master fb0dc3f 切出；纯文档
变更——判定依据为 `M7-09` 已发布基准数字，不重跑、不改写）：

- 判定：**GO（合成口径）**——判定记录见上方"Go/No-Go 判定记录"节
  （六门槛逐项判定表、"仅 D 通过"两项的口径论证与归因、
  `min_compensation_confidence` 库默认维持 0.0 的裁定与测量依据、五项
  判定限定）。门槛初值无一处变更（`DEC-019` 第 5 条）。
- 转正草案：[DEC-022](../decisions/DEC-022-object-tracker-contract-freeze.md)
  （Proposed，待负责人评审）——两阶段拆分（阶段 1 三处 Experimental 面
  契约冻结 / 阶段 2 真实截图评估与转正收口）、证据登记（仅有合成证据、
  不回滚语义）与三项开放项（补偿置信度库默认、PSR 5.0 真实裕度复核
  ——`M7-09` 校准表转记、`RISK-2026-16` 回退是否入库默认）；
  [DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md) 头部补
  反向注记。
- 登记：[兼容性登记](../compatibility/compatibility.md) 与
  [API 索引](../api/README.md) 按两阶段模式注记（M7-10 判定 GO +
  `DEC-022` Proposed；批准前 Experimental 标记与"不计兼容性承诺"登记
  不变，同 M6-06 → `DEC-018` 先例）；总计划 1.23 修订。
- 限制与移交：CHANGELOG 留 M7 收尾（`v0.4.0`）统一处理；本项无代码/测试
  改动，六预设门禁与 CI 证据随编排脚本在分支 head 收口回填；
  `DEC-022` 评审批准、三项开放项裁决与真实截图评估（`RISK-2026-14`，与
  `DEC-018` 阶段 2 共享 `~/mirador-eval/` 采集）待负责人排期。

同日 `M7-10` 验证轮处置（验证员复核判定记录与提交纪律：一项 minor、一项
info 观察，**均无需改动**；判定 commit 与全部文档交付内容维持不变）：

- minor（commit 格式，按先例处置、无需返工）：判定 commit 067f913 的
  subject 为 `docs: …` 无 `(scope)`，字面上不满足 AC5 的
  `<type>(<scope>): <subject>`；但与本工作项同型先例 b05f783（M6-06 判定
  commit，已合入 master）及 master 上至少 8 条 scope-less `docs:` 提交完全
  一致——AGENTS.md 的 scope 枚举亦无 plans/decisions 对应项，验证员判定
  为按仓库既定惯例执行，无需返工。实现侧不重写已交付 commit（改写历史
  无逻辑收益且违背提交纪律），本处置记录及后续提交沿用同一先例格式。
- info（非缺陷观察，留档备查）：判定记录已如实声明"六预设门禁与 CI 证据
  随编排脚本在分支 head 收口回填"（未执行验证已按 AC5 记录原因与补跑
  条件）；验证员本轮按其 ask 范围仅抽查 asan/ubsan、未跑 TSAN——与纯
  文档变更及本项范围一致，留档备查。
- 验证员测试交付：`test(fusion)` commit 8e95371 新增
  `mirador.fusion.object_tracker_verdict_ruling` 套件，钉住本判定
  `min_compensation_confidence` 裁定的行为面——局部变化伪位移带
  [0.46, 0.55] 在库默认 0.0 门下应用（判定记录如实登记的未配置调用方
  暴露面）、在 0.7 调用方策略下显式拒绝（估计回显、池不动）；真滚动带
  [0.93, 0.95] 两种配置下均应用；显式 0.0 配置与库默认行为逐位一致
  （默认值本身仍由 `M7-09` 校准套件钉住）。变异校验（临时上调库默认
  0.7 后两测失败、头文件还原）证实钉住有效。
- 复验（实现侧本地证据，本会话执行）：`cmake --build --preset debug`
  增量 up-to-date（`ninja: no work to do`，验证员测试二进制已构建零告警）；
  debug 全量 ctest 52/52 通过（architecture 7 / property 1 / unit 44，含
  新套件直跑 `mirador.fusion.object_tracker_verdict_ruling` 2/2 通过，
  既有套件零回归）；工作树仅本条处置记录文档变更。六预设完整门禁与
  CI 证据仍随编排脚本收口。

同日 `M7-10` lint 门禁修复与本地门禁证据收口（验证员随验证轮交付的裁定
钉住套件（8e95371）在 CI lint 口径 `clang-tidy --warnings-as-errors='*'`
下报 3 处 error，按仓库测试文件先例修复——`test(fusion)` commit da8af43，
用例名称、数量与断言语义零变化；验证员同步复核处置 commit 840d833：
`git diff 8e95371..840d833 --stat` 仅本里程碑文档 +29 行、零代码/契约
变更，契约面 `min_compensation_confidence` 库默认 0.0
（`object_tracker.hpp:198`）未被触碰）：

- 修复（全部限定在 `tests/fusion/object_tracker_verdict_ruling_test.cpp`，
  5 insertions / 3 deletions，不触碰库实现与公共契约，三类均直接套用仓库
  既有先例而非引入新处置风格）：(1) `misc-unused-using-decls`——移除未
  使用的 `using mirador::MotionCompensationResult`（全文件仅此一处出现，
  结果类型均经 `auto` 消费）；(2)
  `readability-function-cognitive-complexity`（TestBody 182 > 25）——TEST
  行上方 `NOLINTNEXTLINE`（gtest 宏展开主导该指标；
  `object_tracker_motion_generation_test.cpp` 同款先例措辞与位置，M7-07
  验证套件已确立此处置——表驱动用例拆散断言归属反而伤可读性，拆分
  helper 后各 helper 仍超阈值且改变 ASSERT 中止语义归属）；(3)
  `modernize-avoid-c-arrays`——`cases` 表 `BandCase[]` 改
  `std::array<BandCase, 4>`（`object_tracker_redetection_test.cpp` 先例，
  双花括号聚合初始化）并补 `<array>` 头。首轮修复的 NOLINT 注释超 120 列
  被 clang-format 拦截，已缩短为先例原句。
- 门禁证据（修复会话执行，均为修复后复跑）：`clang-tidy
  --warnings-as-errors='*'`（-p build/debug）对该文件修复前复现退出码 1
  （恰为报出的 3 处 error）、修复后退出码 0（Suppressed 43961 warnings、
  18 NOLINT、无 error）；`clang-format --dry-run --Werror` 对该文件退出码 0
  （首轮 166:121 超长违规已修），全仓格式清扫（CI lint job 同口径
  `git ls-files '*.cpp' '*.cc' '*.h' '*.hpp' | xargs`）退出码 0；
  `cmake --build --preset debug` 零告警；debug 全量 ctest 52/52、新套件
  直跑 2/2 PASSED（断言路径与修复前一致）。
- 复跑（文档同步轮，本会话执行，分支 head da8af43）：debug 增量构建
  `ninja: no work to do`；`ctest --preset debug` 52/52 通过
  （architecture 7 / property 1 / unit 44）；新套件直跑 2/2 PASSED；
  clang-format 单文件与全仓 dry-run 均退出码 0；clang-tidy
  `--warnings-as-errors='*'`（-p build/debug）对该文件退出码 0；asan/
  ubsan 目标增量 `no work to do` 后直跑各 2/2 PASSED、sanitizer 零报告。
- 限制：release/warnings/tsan 预设与 asan/ubsan 全量 ctest 未执行——本
  失败面为 debug 可编译纯测试文件的 lint 口径，lint 双口径与 debug ctest
  已覆盖；warnings/tsan 预设与 asan/ubsan 全量 ctest 侧已由 CI 在分支
  head 覆盖（见下方 CI 回填；CI 矩阵无 release 预设，release 侧仍属未
  执行范围，上条验证轮留档的 TSAN 范围限定就此闭合）；tidy 全仓扫描仅
  对 da8af43 所改文件执行，其余文件由 CI lint job 全仓口径覆盖。
- CI 回填：PR #30 单轮 run 全绿，14/14 job——msvc/ninja、ndk/arm64-v8a、
  gcc10（ubuntu-20.04 容器）、gcc debug/asan/ubsan/tsan/warnings 五预设、
  clang debug/fuzz、integrations-ncnn、capture/opencv 适配与
  clang-format/clang-tidy 双口径（lint job 排队后 26m12s 完成）。
  [run 36377952484](https://github.com/Linductor-alkaid/mirador/actions/runs/36377952484)
  （head 8934bee，覆盖判定与转正草案、验证员裁定钉住套件、验证轮处置、
  lint 修复与门禁证据/文档同步 commit，全程 26m16s）；首轮通过，无修复
  往返，上条"CI 回填：待补——六预设完整门禁与 CI 14/14 随编排脚本在
  分支 head 收口后回填（同 `M7-09` 先例）"就此闭合。
- 同步：CHANGELOG Unreleased 补登记 M7-10 判定条目（提前于原定 M7 收尾
  时点；`v0.4.0` 发布说明整理职责不变），判定记录"后续动作"随改；总计划
  `SCOPE-13` 状态标记纠偏——`DEC-019`/`DEC-020` 已于 2026-09-21 经负责人
  批准转 Accepted（1.8 修订已录），正文标记仍为 Proposed，本轮更正；
  API 索引与兼容性登记维持两阶段注记不变（本轮零公共契约变更）。

2026-09-28：`M7-11` `TrackerBackend` SPI 契约冻结交付与验证轮处置，工作项
勾选（分支 `feat/m7-11-tracker-backend-spi-freeze` 自 master 2540ed1 切出，
四个 commit：契约实现 6bd7523 → 文档注册 ceafa3b → 验证套件 3db5d1d →
验证轮修复 4508da7；验证套件由 Independent-Verification-Agent 按分工
交付）：

- 交付：`include/mirador/tracker_backend.hpp`（Experimental，`DEC-020`，
  269 行纯接口头文件，零新增链接依赖，CMake 零注册变化——经既有 include
  接口收录，同 `detector_backend.hpp`）——`TrackerBackend::initialize` →
  `TrackerSession::update` 句柄制三段式生命周期（一会话一目标、重初始化即
  新会话、调用方对后端保活、同后端会话相互独立）；状态归属会话、析构即
  显式丢弃；`kBackendFailure` 失败可见不返回陈旧结果（弃置会话重建恢复，
  fusion 侧经既有冻结原语降级 `kUncertain`）、`kCancelled`/`kTimeout` 会话
  保持可用、update 全有或全无；冻结裁定**校验先于取消**（M7-05/06 池
  先例，与 M7-02 取消优先入口刻意对照）；并发：多句柄并发性由
  `BackendInfo.thread_safe` 显式声明、单句柄严格串行（`RULE-03` 适用不
  豁免，后端不建线程/定时器，runtime 内部异步在返回前完成）；缓存豁免
  界定：会话状态不进能力结果缓存、`RULE-07`「相同输入相同结果」对
  `update` 显式不成立、仅 `initialize` 确定性前处理产物可按 `RULE-07`
  键缓存；字段集 `TrackerInitRequest{initial_bounds, backend_params}`/
  `TrackerUpdateRequest{prior_bounds, backend_params}`（位置先验每帧由
  调用方供给，会话状态仅外观）/`TrackerUpdateResult{bounds, confidence}`
  按 `DEC-021` 作不可信输出消费；坐标与输入语义同 `DEC-012`（prepared
  像素空间、`accepted_formats` 门控、不修改输入）；逐序列位确定性；
  隐私 `RULE-10`/`DOD-06`。fusion 侧句柄槽沿池侧并行单槽先例
  （M7-05/06/08 槽模式）、槽开销计入 `byte_size()`/`pool_budget_bytes`、
  terminate/淘汰/reset 同步析构的归属决策冻结于头注释（槽常量本体随
  M7-13 注入接线落地）。
- 文档注册（ceafa3b，契约冻结要求的登记面）：[API 索引](../api/README.md)
  core SPI 表新增 `tracker_backend.hpp` 条目（含 `DEC-012`「相同输入相同
  结果」对 `update` 显式不适用的范围说明——`DEC-020` 要求的索引注记）；
  [兼容性登记](../compatibility/compatibility.md) 第四处 Experimental 面
  登记（同"不计兼容性承诺"措辞，并标记 `DEC-022` 阶段 1 目前仅列三处、
  本面须随 M7 收尾/其评审一并纳入冻结范围——待负责人处置）；
  [DEC-012](../decisions/DEC-012-backend-spi-contract.md) 决策 6 增适用
  范围注记（只界定范围，不改任何冻结原文）；跟踪设计 §6.2 增冻结落地
  注记（句柄生命周期、字段集、校验先于取消裁定、句柄槽字节记账归属与
  M7-12/M7-13 边界）；CHANGELOG Unreleased 登记 M7-11 契约条目。
- 验证套件：`tests/fusion/tracker_backend_orchestration_test.cpp` 20 用例
  （注册 ctest 项 `mirador.fusion.tracker_backend_orchestration`，LABELS
  unit，链接 `mirador::fusion` 零新依赖）随 test(tests) commit 3db5d1d
  落地——伪实现固定轨迹注入后端排演 M7-13 fusion 侧接线（零新增
  `ObjectTracker` API，全部消费已冻结原语），覆盖：`BackendInfo` 身份与
  `validate` 门控；initialize/update 校验矩阵（错误路径会话状态与轨迹
  指针不动）；冻结顺序校验先于取消（已取消且过期上下文下畸形请求仍报
  `kInvalidArgument`）；取消/超时显式转化后会话完全可用（全有或全无）；
  固定轨迹逐成功 update 注入回放；`kBackendFailure` 可见不陈旧、冻结
  轨迹游标、弃置重建恢复（`RISK-2026-18` 挂钩）；失败降级经
  `commit_track_evidence` 占位级走 kTracking → kUncertain；同后端会话
  独立；terminate/池淘汰/reset 三路句柄同步析构；M7-13 槽常量落地前
  SPI 编排零池字节（`byte_size` 检查点与无后端基线相等）；会话状态不进
  能力结果缓存负向（同会话相同 update 实参产出不同结果、`RULE-07` 键
  相同——以真实 `CapabilityResultCache` 演示拦截将服务陈旧首帧结果）；
  DOD-03 坐标矩阵（0/90/180/270 旋转元数据 × 奇数呈现尺寸 × 非连续
  stride × 贴边先验，同呈现内容逐位一致、输入字节零修改）；跨会话跨
  后端实例逐序列位确定性；全编排零落盘、Status 消息无像素内容标记
  （`RULE-10`）。
- 验证轮处置：验证员复核发现一处 minor 契约文本歧义，实现侧修复随
  docs(core) commit 4508da7 落地——契约块 5 对可缓存 `initialize` 前处理
  产物的 `RULE-07` 键构成原枚举（"backend name, implementation version,
  model id/revision and a digest of the request parameters"）易被读作
  穷尽列举而遗漏 prepared 图像内容维度，而该产物恰由 prepared 图像派生
  （缺图像摘要的模板缓存会命中陈旧模板——`CapabilityKeyFields::
  image_fingerprint` 存在的同一原因，`capability_cache.hpp`，设计 §12；
  缓存层在调用方侧，架构测试无法拦截该实现错误）。修复在两处携带该
  枚举的表面同步改写为"按 `RULE-07` 对产物派生自的全部输入覆盖"并显式
  列入 prepared 图像内容摘要（tracker_backend.hpp:107-113 契约块 5、
  `DEC-012` 决策 6 注记括号枚举 DEC-012-backend-spi-contract.md:53-56
  ——同源缺陷的两处实例一并修复保持一致），属完备性澄清（把 `DEC-020`
  决策 4「`RULE-07` 键构成适用」从可宽泛解读变为不可误读），非放宽亦非
  新增约束——纯注释修改，零签名/语义变更，未放宽公共契约；`DEC-020`
  Accepted 决策原文不动（头文件是其"M7-11 定稿"条款指定的字段权威面，
  歧义在该面消解）。修复前后套件 20/20 原样通过（未触及任何被钉住的
  签名/语义/编排行为，无测试过时）。
- 门禁（文档同步时点于分支 head 4508da7 复验，本会话执行）：
  `cmake --build --preset debug` 增量 up-to-date；debug 全量 ctest 53/53
  通过（label 汇总 architecture 7 / property 1 / unit 45，既有套件零
  回归）；编排套件 ctest 1/1、直跑 20/20 PASSED；`ctest -L architecture`
  7/7（source_scan + 6 个链接闭包探针；fusion 探针 NEEDED 实测恰为
  libstdc++/libm/libgcc_s/libc——core 链接闭包不变，零新依赖）；
  clang-format `--dry-run --Werror` 对头文件与测试文件归零；clang-tidy
  `--warnings-as-errors='*'`（`-p build/debug`）对头文件（37,437 条
  suppressed 均为非用户代码）与测试文件（61 处 NOLINT）退出码均 0；
  asan/ubsan 预设增量 up-to-date 后编排套件各 1/1、sanitizer 零报告。
  验证轮修复会话已证（4508da7 记录）：头文件变更触发全量重编零告警、
  修复后 debug ctest 53/53 与架构 7/7；asan/ubsan 预设重建零告警后
  编排套件直跑通过、零 sanitizer 报告（验证轮会话报告）。
- 限制：release/warnings/tsan 预设与 asan/ubsan 全量 ctest 未在文档
  同步轮执行（注释级修复不触达编译产物；warnings/tsan 预设与
  asan/ubsan 全量 ctest 侧已由 CI 在分支 head 覆盖，见下方 CI 回填；
  CI 矩阵无 release 预设，release 侧仍属未执行范围，六预设完整复跑
  随编排脚本收口，同 M7-03~10 先例）；`DEC-022` 第四处
  Experimental 面纳入冻结范围与 `DEC-021` known_limitations 落地载体
  为前轮既登记 concerns，本项不改变其状态（随 M7 收尾/`DEC-022` 评审
  处置）；NanoTrack 参考后端（M7-12）与深度增强注入接线（M7-13）不在
  本项——本套件的伪实现即二者可执行规格。
- CI 回填：PR #31 单轮 run 全绿，14/14 job——msvc/ninja、ndk/arm64-v8a、
  gcc10（ubuntu-20.04 容器）、gcc debug/asan/ubsan/tsan/warnings 五预设、
  clang debug/fuzz、integrations-ncnn、capture/opencv 适配与
  clang-format/clang-tidy 双口径（lint job 40m4s 完成，主导全程）。
  [run 36390979653](https://github.com/Linductor-alkaid/mirador/actions/runs/36390979653)
  （head 4771b62，覆盖契约冻结、文档注册、验证套件、验证轮修复与
  门禁证据/文档同步全部 5 个 commit，全程 40m7s）；首轮通过，无修复
  往返，上条"CI 回填：待补（分支未推送，推送与 14/14 证据回填随编排
  脚本收口，同 `M7-09`/`M7-10` 先例）"就此闭合。

2026-09-28：`M7-12` NanoTrack ncnn 参考后端**实现与审查轮交付**（分支
`feat/m7-12-nanotrack-ncnn-reference-backend`，工作项保持未勾选——合成
模型冒烟套件按分工由独立验证工程师随后续 commit 交付，勾选随其落地与
CI 证据回填一并处置）。本会话（实现工程师）记录：

- 许可证与模型来源审查（工作项第一步与门槛）：**通过**。候选 port
  HonglinChu/NanoTrack @ `76b1c6711ae11958ec592e525163826d357d5db5`
  （master head，2023-06-08，GitHub API 核对），仓库根 `LICENSE` 为
  Apache-2.0（允许再分发与构建树获取，含专利授权）；模型权重为 port
  仓库内 assets（port 作者转换、随仓库同许可分发），本仓库不入仓、不
  下载、不随任何机制分发，真实权重由使用者显式路径提供（`DEC-015`
  分层、`RISK-2026-13` 口径）。对 pinned ncnn `e54f7b1f`（20260526）
  兼容核对：候选模型仅标准层（Input/Convolution/Pooling/Concat，无自定义
  层），pin 参数字典完整覆盖，**未升级 ncnn pin**。实现路线照 M5-04
  YOLO 先例取"冻结模型输出契约"而非构建期引入 port：`integrations/`
  代码为自研，对 port 零 FetchContent/零编译/零链接——因此
  `deps.lock.json` 无新条目（工作项文本"若以上游仓库 FetchContent 获取"
  条件未触发），登记为文档性对齐 + 供应链审计记录。审查未触发
  `DEC-020` 备选更换（LightTrack/Ocean），候选维持 NanoTrack。审计
  记录 `docs/supply-chain/nanotrack.md`（来源/精确 commit/许可证/获取
  方式/使用范围/兼容核对/验证/更换流程）+ `dependency-policy.md`
  依赖清单行。
- 交付：`integrations/tracker_nanotrack/`（`mirador_tracker_nanotrack`
  库，链接 `mirador::core`/`mirador::image`/`mirador_ncnn_runtime`，
  `mirador_apply_warnings`，`integrations/CMakeLists.txt` 末尾
  add_subdirectory）——`NanoTrackerOptions`（四路径显式、身份字段流入
  `BackendInfo`、`num_threads` **显式限定 1**（其他值 `create` 报
  `kInvalidArgument`：逐序列位确定性为契约块 8 硬要求）、
  `work_budget_bytes` 默认 16 MiB）+ `NanoTrackerBackend`
  （`info()`/`initialize`）+ 会话（模板特征 + 参考尺寸，`NcnnRuntime`
  以引用共享后端 runtime——契约块 1 调用方保活条款使然）。逐契约块
  1-9 对照实现：validate → accepted_formats（kRgb8）→ bounds 矩阵的
  校验先于取消（块 4）；update 全有或全无（解码全部局部量、成功才提交
  参考尺寸，块 3）；`kBackendFailure` 失败可见（响应图形状违约、退化
  框、无可用 cell、非有限值均显式；wrapper 的"未知 blob
  kInvalidArgument"在本后端语境重映射为 `kBackendFailure`——模型契约
  违约不是 SPI 调用方错误）；`kCancelled`/`kTimeout` 入口 + 循环定期
  检查、会话保持可用；rotation 元数据永不解读、奇数尺寸/非连续 stride
  经 presented 坐标正确读取、输出钳制于 prepared 像素空间（块 6，
  DEC-021 拒绝而非伪造）。冻结参考解码（尺寸/比率惩罚 + cosine 窗 +
  行主序首最大总序 + ltrb 逆 crop 映射 + 单次尺寸学习率规范化）以
  后端头注释为权威；位置先验逐帧来自 `prior_bounds`，后端不自造位置。
- `NcnnRuntime`（M5-02 共享面）扩展：新增 `NcnnNamedTensor` +
  `run_multi`（head 双输入双输出所需；对 pinned ncnn 实测 **extractor
  单输出语义**——同 extractor 第二次 extract 返回 -100，故逐输出独立
  extractor 重放相同输入，输出间一致）；`opt.use_packing_layout=OFF`
  （packing 会折叠小通道数，破坏本包装"平面 CHW 张量"文档契约，多
  blob 链会把交错平面误读为通道数——实测复现后修正）；`run` 改 const
  并委托 `run_multi`（会话持有共享 runtime 引用所需；三个既有冒烟
  ncnn/ppocr/yolo 全部原样通过）。
- 门禁（本会话于实现分支 head 执行）：integrations 构建
  （`MIRADOR_BUILD_INTEGRATIONS=ON`，FetchContent pinned ncnn 经本机
  代理）成功零告警，`ctest` 55/55（含既有 ncnn/ppocr/yolo 冒烟与全部
  核心套件）；debug 默认预设 `cmake --preset debug` + build 增量
  up-to-date、全量 ctest 53/53 零回归（默认构建图零获取口径不变，
  架构测试未触）；clang-format `--dry-run --Werror` 对四个新增/改动
  integrations 文件归零；clang-tidy `--warnings-as-errors='*'`
  （`-p build/integrations`）对 `nanotrack_backend.cpp` 与
  `ncnn_runtime.cpp` 归零（9 处告警修复后复跑）。实现自证：合成双模型
  （常量峰 + 内容驱动峰两种）驱动后端全链路 43 项断言通过（精确解码
  数值、跨会话逐序列位一致、0/90/180/270 × stride 填充位一致、输入
  字节零修改、校验先于取消、预算负路径、失败可见）。
- 限制与待办：合成模型冒烟套件
  （`mirador.integrations.tracker_nanotrack_smoke`）与 `MIRADOR_BUILD_
  TESTS` 条件测试 target 由独立验证工程师按分工交付（涵盖 initialize/
  update 快乐路径、校验矩阵、预算负路径、失败可见与会话重建、确定性、
  DOD-03 矩阵、隐私负向；CI 既有 integrations-ncnn job 的 ctest 自动
  捕获，job 本体零改动）；验证轮独立复核、全量六预设、sanitizer 与
  PR/CI 14/14 证据随其收口。真实权重评测与跟踪质量声明不在本项
  （`DOD-05`/`RISK-2026-13`：归 M7-13 D+ 列报与后续真实评估）；合成
  口径结论仅限管线与解码正确性。
- 验证轮处置（2026-09-28，验证套件 839dfd7 交付后的实现侧修复，随
  fix(integrations) commit 落地）：验证员三项发现逐项收口，零公共契约
  变更——(1) RULE-06 记账口径：按验证员给出的两路收口选项取**文档措辞
  精确化**（`work_budget_bytes` 头注释、`WorkBudget` 类注释与 update
  记账点注释三处同步改写为精确边界：后端自身可控的分配请求——crop 暂存、
  forward 输出张量、模板状态、解码窗——逐请求预算检查；wrapper 侧
  per-forward 输入拷贝与 ncnn 内部中间/工作区缓冲受冻结模型契约几何
  约束、非无界、不可自后端面单独计账，沿 M5 既有后端同款口径）。实现
  过程曾按"记账收口"路线为 forward 输入拷贝追加计账，但验证套件的
  预算夹具（starved 3000 / tight 8000）钉住计账集，追加计账使
  initialize 总量 5184→8256 字节、套件"initialize fits the 8000-byte
  budget"正例失败——为不弱化验证套件的独立性（夹具归验证员维护），
  回退追加计账、维持文档收口路线。(2) 格式门重复定义漂移：会话构造时
  从后端 `info_.accepted_formats` 派生 `accepted_formats_` 成员，update
  与 initialize 校验同一列表，文件级静态 `kAcceptedFormats` 删除——
  行为不变（值恒等），漂移面消除。(3) 钳制语义：按验证员定位为
  observation 的口径**文档化而非改行为**——头注释新增"冻结钳制语义"
  段：上报 bounds 为预测框钳入 prepared 图（契约块 6/DEC-021），参考
  尺寸更新**刻意以钳制后尺寸为目标**（尺度状态追踪帧内可观测部分，
  防止目标部分出图时搜索窗按不可观测预测持续放大），"以未钳制预测
  尺寸入状态"的替代方案列为 M7-13 位置证据接线时与设计 §6.2 的显式
  对齐点（属行为变更，非措辞修正）。修复后门禁（本会话执行）：
  integrations 全树 ctest 56/56 连续三轮（含验证套件 153 检查项）、
  debug 预设 53/53 零回归、clang-format/clang-tidy 双口径对
  nanotrack_backend.{hpp,cpp} 归零、实现侧合成模型 harness 全部断言
  通过（SANITY PASS）。
- 环境干扰记录（验证轮须知）：本机全量 ctest 复跑中两次观察到
  `mirador.privacy.privacy`（`Privacy.PipelineWritesNoFiles`）与一次
  `mirador.fusion.tracker_backend_orchestration`（同名快照校验模式）
  间歇失败，根因经复现捕获确认：外部进程在本机 `/tmp` 持续创建
  `mirage-m1-04-*` 临时文件（非本仓库任何代码产物，仓库全文检索零
  命中，创建时间与调用方 PID 持续更新），落入"快照前后目录比对"的
  窗口即误报新文件——为共享 `/tmp` 假设的既有测试设计敏感点，与本项
  改动无关（涉事二进制不链接 integrations 代码；直跑二进制 200 次
  零复现，仅全树 ctest 间发）。验证轮在本机复跑门禁遇此失败时，先
  核对 `/tmp/mirage-*` 是否新增再归因；硬化（私有临时目录或按前缀
  过滤）归测试面负责方另行处置，不在本项分支混入。

2026-09-28：`M7-12` 合成模型冒烟套件交付、验证轮三项发现收口与工作项
勾选（分支 `feat/m7-12-nanotrack-ncnn-reference-backend`，七个 commit：
实现 2587eaa → `run_multi` 3a8c89d → 供应链审查登记 4f43aa8 → /tmp
干扰记录 ec52b75 → 验证套件 839dfd7 → 验证轮修复 48de7db + 处置记录
df63c17；验证套件由 Independent-Verification-Agent 按分工交付，实现与
审查轮交付见上方两条记录）：

- 交付摘要：验证套件 `integrations/tracker_nanotrack/test/nanotrack_smoke.cpp`
  （839dfd7，1439 行，153 检查项）注册 ctest 项
  `mirador.integrations.tracker_nanotrack_smoke`（`MIRADOR_BUILD_
  INTEGRATIONS` + `MIRADOR_BUILD_TESTS` 条件，同 detector_yolo/
  ocr_ppocr 冒烟形态；CI integrations-ncnn job 的 ctest 自动捕获，
  job 本体零改动），六套运行时生成合成 ncnn 模型（零权重入树）端到端
  钉住冻结契约。验证轮三项发现随 fix(integrations) 48de7db 逐项收口
  （处置明细见上方"验证轮处置"段），零公共契约变更：(1) `RULE-06`
  记账口径取验证员两路收口选项中的**文档措辞精确化**路线——
  `work_budget_bytes` 头注释（nanotrack_backend.hpp:59-71）、
  `WorkBudget` 类注释（nanotrack_backend.cpp:49-61）与 update 记账点
  注释（:501-511）三处同步改写为精确边界（后端自身可控分配——
  crop 暂存/forward 输出张量/模板状态/解码窗——逐请求计账；wrapper 侧
  per-forward 输入拷贝与 ncnn 内部中间/工作区缓冲受冻结模型契约几何
  约束、非无界、沿 M5 口径不可自后端面单独计账）；曾按"追加计账"路线
  实现后回退——验证套件预算夹具（starved 3000 / tight 8000）钉住
  计账集，追加计账使 initialize 总量 5184→8256 字节令其
  "initialize fits the 8000-byte budget" 正例失败，夹具归验证员维护
  不弱化。(2) 格式门漂移消除——会话构造时从后端 `info_.accepted_formats`
  派生 `accepted_formats_` 成员（nanotrack_backend.cpp:456、:694-696），
  update（:469）与 initialize（:647）校验同一列表，文件级静态
  `kAcceptedFormats` 删除（grep 零命中）——行为不变（值恒等），两门
  同源。(3) 钳制语义按验证员 observation 定位**文档化而非改行为**——
  头注释新增"冻结钳制语义"段（nanotrack_backend.hpp:121-130：上报框
  为预测框钳入 prepared 图、参考尺寸刻意以钳制后尺寸为目标——尺度
  状态追踪帧内可观测部分），"以未钳制预测尺寸入状态"列为 M7-13 与
  设计 §6.2 的显式对齐点（属行为变更，非措辞修正）。
- 验证覆盖（153 检查项）：精确解码对照手推边界与独立 double 精度重放
  （胜者扫描、ltrb 映射、钳制、置信度、参考尺寸单次更新）、调用方位置
  先验所有权、校验矩阵与冻结的校验先于取消顺序、kCancelled/kTimeout/
  kInvalidArgument/kUnsupportedFormat 全有或全无、initialize/update
  双路径 `kBudgetExceeded`、`kBackendFailure` 可见（无可用 cell、出图
  解码、错误 blob 名经 wrapper 错误重映射、3 通道 cls 形状门）、跨会话
  跨后端逐序列位确定性、经 crop/backbone/head 的像素敏感性与窗外/
  patch 外不变性、0/90/180/270 旋转 × 非连续 stride × 奇数尺寸矩阵、
  输入字节零修改、隐私（零落盘、Status 消息无像素标记）；wrapper 级
  `run_multi` 精确输出、参数校验与 moved-from 路径。修复为零公共契约
  变更（措辞精确化 + 构造期派生同值列表），153 检查项在 HEAD 原样重跑
  通过——测试未过时、零改动。
- 门禁（验证轮会话于分支 head df63c17 执行，随处置记录 df63c17 留档）：
  153 检查项直跑 5 次全 PASS（exit 0）、`ctest -R tracker_nanotrack`
  Passed；integrations 全树 ctest 共 7 轮——5 轮 56/56 全绿，2 轮各
  1 失败均归因上段环境干扰记录的共享 `/tmp` 外部进程（本机确认 18 个
  `/tmp/mirage-m1-04-*` 外部产物，涉事套件为文档点名的无过滤 /tmp
  前后快照模式、仅链接 core 头 + fake 后端，单跑即 Passed）；debug
  预设 build 增量 up-to-date + 全量 ctest 53/53 零回归；clang-format
  `--dry-run --Werror` 与 clang-tidy `--warnings-as-errors='*'`
  （`-p build/integrations`）对 nanotrack_backend.{hpp,cpp} 与
  nanotrack_smoke.cpp 归零；二进制新鲜度核查（object 时间戳晚于修复后
  源，ninja no work to do）；sanitizer 抽查——`-fsanitize=address,
  undefined` 重建 smoke + 修复后 backend + wrapper 链接 libncnnd.a，
  153 检查全 PASS 零报告；实现侧合成模型 harness（/tmp，未入仓）
  SANITY PASS。
- 文档同步轮复验（本会话执行，分支 head df63c17）：integrations 构建
  增量 `ninja: no work to do`；`ctest -R tracker_nanotrack` 1/1
  Passed（冒烟二进制直跑 PASS、exit 0）；integrations 全树 ctest
  56/56；`cmake --build --preset debug` 增量 up-to-date + 全量 ctest
  53/53；clang-format `--dry-run --Werror` 对 nanotrack_backend.{cpp,
  hpp} 与 nanotrack_smoke.cpp 三文件退出码 0；clang-tidy
  `--warnings-as-errors='*'`（`-p build/integrations`）对
  nanotrack_backend.cpp 与 nanotrack_smoke.cpp 退出码 0（Suppressed
  88936、15 NOLINT）。
- 限制：(1) 发现 1 为**措辞收口**而非记账收口——`work_budget_bytes`
  不覆盖 wrapper 内部 per-forward 输入拷贝与 ncnn 内部中间/工作区缓冲
  （受冻结模型几何约束、非无界、M5 同款口径）；如编排侧/负责人倾向
  严格计账，改动点为 `update()`/`initialize()` forward 前三处 charge
  + 验证套件预算夹具同步（8000→≥8256），属行为变更需验证轮复验。
  (2) 钳制语义（状态吃进钳制尺寸）已文档化为冻结选择，与设计 §6.2 的
  最终对齐归 M7-13——若 M7-13 确认改为未钳制预测尺寸入状态，属行为
  变更需验证轮复验（边缘帧数值会变）。(3) 全量六预设/sanitizer 全量
  ctest 不在本轮（编排脚本职责，同前轮口径）；warnings/tsan 预设与
  asan/ubsan 全量 ctest 侧已由 PR CI 在分支 head 覆盖（见下方 CI 回填；
  CI 矩阵无 release 预设，release 侧仍属未执行范围）；本分支实际
  执行面以上两条门禁段为准。(4) 共享 `/tmp` 外部进程（`mirage-*`）
  间歇干扰的既有记录（ec52b75）仍有效，本轮文档同步轮复跑未再遇
  （全树与抽验均绿）。(5) 真实权重评测与跟踪质量声明不在本项
  （`DOD-05`/`RISK-2026-13`：归 M7-13 D+ 列报与后续真实评估），合成
  口径结论仅限管线与解码正确性——沿用实现轮记录。
- CI 回填：PR #32 单轮 run 全绿，14/14 job——msvc/ninja、ndk/arm64-v8a、
  gcc10（ubuntu-20.04 容器）、gcc debug/asan/ubsan/tsan/warnings 五预设、
  clang debug/fuzz、integrations-ncnn（5m40s，ctest 自动捕获
  `mirador.integrations.tracker_nanotrack_smoke` 冒烟套件）、capture/
  opencv 适配与 clang-format/clang-tidy 双口径（lint job 38m49s 完成，
  主导全程）。[run 36420344189](https://github.com/Linductor-alkaid/mirador/actions/runs/36420344189)
  （head 4e35f8e，覆盖实现、run_multi、供应链审查登记、/tmp 干扰记录、
  验证套件、验证轮修复、处置记录与勾选/文档同步全部 8 个 commit，全程
  38m52s）；首轮通过，无修复往返，上条"CI 回填：待补——分支未推送，
  推送与 PR/CI 14/14 证据（integrations-ncnn job 的 ctest 自动捕获
  冒烟套件）随编排脚本在分支 head 收口后回填（同 `M7-03`~`M7-11`
  先例）"就此闭合。

2026-09-28：`M7-13` 深度增强通道条件化融合**实现交付**（分支
`feat/m7-13-deep-channel-conditional-fusion` 自 master bde1c6a 切出；工作项
未勾选——验证套件按分工由 Independent-Verification-Agent 独立编写与执行，
勾选、验证轮记录与 CI 证据随其收口）：

- 交付（实现 feat(fusion) commit 404d339 + bench feat(benchmarks)
  commit 696bf48 + 文档同步）：`ObjectTracker` 新增注入点原语
  `attach_tracker_session`/`detach_tracker_session`/`tracker_session`
  （池侧并行单槽存调用方初始化的 `TrackerSession` 句柄，兑现 M7-11
  契约块 2「槽常量随 M7-13 注入接线落地」注记）——每 track 至多一槽、
  重建原地替换（destroy 旧存新、字节中性）、`kTrackerHandleSlotOverheadBytes=32`
  计入 `byte_size()`/`pool_budget_bytes` 放不下显式 `kBudgetExceeded`
  （错误路径池与会话均不动）、terminate/池淘汰/reset 三路同步析构句柄、
  `plan_eviction`/adopt 淘汰提交/归档路径同步释放槽字节；池从不驱动
  后端（initialize/update、prepared 视图制备、坐标恢复全归调用方，
  `RULE-12` 上层同时拥有注入与 `deep_channel_enabled` 启用开关）。
  `commit_track_evidence` 深度证据重载（`DeepChannelEvidence`）：
  `DEC-021` 采纳点校验——非有限/零面积/出图 bounds、非有限/出 [0,1]
  置信度、开关关闭时供证，一律显式 `kInvalidArgument` 拒绝而非钳制，
  校验先于取消（M7-05/06 冻结顺序）；冻结组合规则（头注释权威）：
  深度框与 E1 候选窗（M7-06 冻结候选规则）IoU ≥
  `deep_agreement_min_iou`（默认 0.5）且深度置信 ≥
  `deep_min_confidence`（默认 0.5，均为开发冒烟值）为同位一致互证
  ——确认级置信升级为传统来源（E1 峰值 NCC 或 E2-only 维持原值）与
  深度置信的不下取最大值；否则冲突按保守侧处置（`RISK-2026-18`）——
  确认级降为占位级（冻结状态机 kTracking → kUncertain，不动位置/置信/
  模板），非确认级不受影响（否决语义保留）；深度通道永不单独确认
  （漂移深度 tracker 无法伪造确认，模板补丁恒取自 E1 候选窗——漂移
  结构性不可达外观存储）；高置信模板更新仅取双通道一致帧——开关开启
  且 track 持有注入会话时 kConfirmed 正模板捕获要求本帧互证成立，缺失
  深度证据（后端失败/取消/调用方跳过）仍确认身份但模板捕获扣留并
  显式上报 `template_withheld_by_deep_channel`（`RULE-06` 不静默）；
  提交回显新增 `deep_disposition`；`kBackendFailure` 弃置会话经既有
  占位提交路径降级、当前帧确认 bounds 重建（无新降级 API，M7-11
  冻结钩子兑现）；组合规则作用于 §6.2 邻域验证路径，M7-08 重检测身份
  复核维持冻结形态（其提交不带深度证据，持会话 track 同落模板防护）。
  bench 追加 `D+deep` 第五方法列：确定性伪深度后端（bench-deep 像素
  回声、零学习状态、逐序列位确定）经注入点接入，deep update 在
  verify_track 计时器之外保证 p50/p95 与 D 可比；deep=/corr=/confl=/
  withheld= 计数入逐 cell 指标与确定性摘要；A/B/C/D 打印格式未动。
- 零变化对照（本会话执行，实现 commit 404d339 上）：debug 预设全量
  `ctest --preset debug` 53/53 通过（含 7 个既有 fusion 跟踪套件与
  架构 7/7：源码扫描 + 全链接闭包探针——`mirador_fusion` 链接接口恰为
  core/image/cache 不变，`tracker_backend.hpp` 纯接口零新链接依赖）；
  M7-09 harness 24 个传统 cell 的非计时数字逐 cell 对照已发布报告
  逐位一致（延续率、swap、fp、误判丢/失、verify 调用数、池峰值字节、
  track 峰值——如 A-static 234/234 33,252B、B-static verify=0 7,716B、
  A-scroll 39,960B ×12 等，计时列为墙钟口径不逐位比对）。
- 文档同步：API 索引 object_tracker 条目、兼容性登记 object_tracker
  行扩展、CHANGELOG Unreleased、设计 §6.2 新增 M7-13 交付落点与
  NanoTrack 钳制语义对齐裁决（**维持钳制语义零行为变更**：组合规则只
  消费上报框与置信度，不触碰后端内部尺度状态——`nanotrack_backend.hpp`
  冻结段注记同步收口）、设计 §6.5 M7-13 注记（直通契约零变化）、
  总计划 1.27 修订。
- 待收口：验证套件（验收矩阵 1-11：注入生命周期与字节记账、DEC-021
  校验拒绝矩阵、同位互证/冲突降级/模板防护/失败弃置重建、未注入与
  关闭开关零变化负向、DOD-03 坐标矩阵、DOD-04 不放宽、确定性、隐私、
  D+ 列报口径）由 Independent-Verification-Agent 独立交付；六预设
  构建+ctest、sanitizer、lint 双口径与 CI 14/14 由编排脚本在分支 head
  统一收口；基准 D+ 节已随实现轮列报于
  [linux-x64-object-tracking-2026-09.md](../benchmarks/linux-x64-object-tracking-2026-09.md)
  （合成口径限定、A/B/C/D 传统 24 cell 零回归对照与列报口径依据随附，
  本机 release 复跑 3 次重复非计时指标逐位一致）。
