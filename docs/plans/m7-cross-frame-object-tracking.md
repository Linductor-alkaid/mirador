# M7：跨帧目标跟踪（低负载 SOT 与级联重检测）

> 状态：Proposed（立项随 [DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md) 评审）
> 负责人：linductor
> 所属计划：[Mirador 实施总计划](mirador-implementation-plan.md)（`SCOPE-13`）
> 前置：M6（已完成）；建议与 `DEC-018` 阶段 2 的真实数据评估协调排期，但不互为前置
> 建议发布点：`v0.4.0`（暂定，随判定收尾确认）
> 更新日期：2026-09-21

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

- [ ] `M7-01` 契约冻结：`TrackState`/`TargetTrack`/`ObjectTracker` 公共契约
  （`mirador::fusion`，Experimental 标记）与全部默认值（目标数、位置历史上限、
  模板/负模板数、字节预算、`uncertain_frame_limit`、验证阈值初值、重检测
  退避初值），附伪实现测试与确定性/取消/错误语义说明；同步 API 索引与
  兼容性 Experimental 登记。
- [ ] `M7-02` 目标池有界结构：track 生命周期管理、LRU/最旧优先显式淘汰、
  按布局代际分组的位置历史、模板与负模板存储；预算超限为显式淘汰并计入
  trace（`RULE-06` 负向测试：超预算不静默增长、不静默丢弃）。
- [ ] `M7-03` 变化检测门控三级短路：画面未变/变化 ROI 不相交的近零路径、
  ROI 相交触发的验证入口；同帧多 track 独立短路；短路路径不引入相对 M1
  变化检测基线的可测回归（基准对照）。
- [ ] `M7-04` 全局位移估计原语（`mirador::image`）：低分辨率平移搜索、
  纯 CPU 确定性、预算保护；输出位移向量 + 置信度；坐标链经 `Transform2D`
  组合并通过方向/奇数尺寸/往返容差矩阵（`DOD-03`）。
- [ ] `M7-05` 邻域验证器：验证 ROI 内模板 NCC（多模板最优 + 峰旁瓣质量）
  与 `GeometricRegionProposal` 闭合结构一致性（描述量与池内基线偏差容差）
  双通道；模板版本/参数变化使验证结果失效（`DOD-04` 负向）。
- [ ] `M7-06` 证据融合与状态机：静止/补偿后滚动/代际切换的条件化权重，
  E1/E2/位置/语义四级证据分级（确认/临时延续/占位/否决，含 impostor 负
  模板排除）；`kTracking/kUncertain/kLost/kTerminated` 转移与 `DEC-010`
  tracker 对接（已确认 track 门控直通）。
- [ ] `M7-07` 全局运动补偿与布局代际集成：全局变化分类 → 代际递增 → 位置
  先验条件化（清零/降权）、track 降级与超阈值转 `kLost`；补偿后位置恢复
  验证（滚动场景往返）。
- [ ] `M7-08` 级联重检测原语与身份复核：退避序列、最大重试、变化门控联动
  （静止画面零触发负向测试）、预算耗尽显式失败；重检测候选经池模板 + E2
  复核后延续/新分配 ID 并记录中断事件；策略决策权留给上层（`RULE-12`）。
- [ ] `M7-09` 合成验证 harness 与基准发布：A/B/C/D 方法对比矩阵、第 3 节
  指标全套数字、场景覆盖 `*-static-page`/`*-scroll`/`*-dialog`/
  `*-theme-switch`/`*-similar-icons`/`*-partial-anim`；数字发布于
  `docs/benchmarks/`（`DEC-011` 口径）；门槛初值逐项校准冻结。
- [ ] `M7-10` go/no-go 判定与转正决策草案：依 M7-09 数字对照 `DEC-019`
  门槛逐项判定并记录于本里程碑"验证记录"；GO 另立决策冻结
  `ObjectTracker` 契约并纳入兼容性承诺；NO-GO 记录结论、归因与重跑或关闭
  建议（`DEC-017` 模式）。
- [ ] `M7-11` `TrackerBackend` SPI 契约冻结（`DEC-020`）：接口形态、状态
  归属与生命周期、同步/取消语义、缓存豁免界定；Fake `TrackerBackend`
  （固定轨迹注入）完成 fusion 侧编排测试；架构测试证明 core 链接闭包
  不变；API 索引与兼容性 Experimental 登记。
- [ ] `M7-12` NanoTrack ncnn 参考后端（`integrations/`）：许可证与模型
  来源审查并登记 `docs/supply-chain/`（审查未通过则按 `DEC-020` 备选更换
  候选，契约不变）；复用 `NcnnRuntime` 与 `MIRADOR_BUILD_INTEGRATIONS`
  默认 OFF；合成模型冒烟入 `integrations` 套件与 CI job；真实权重评测按
  `DEC-015` 分层走用户显式路径（`RISK-2026-13` 口径）。
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
- [ ] M7-09 基准发布且门槛逐项判定；M7-10 判定记录完成。
- [ ] 文档同步：API 索引（Experimental 节）、兼容性登记、CHANGELOG、总计划
  `SCOPE-13`、设计文档 §24 M7 状态。

## 验证记录

（按日期追加；立项时尚无实施记录。）
