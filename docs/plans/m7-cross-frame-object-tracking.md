# M7：跨帧目标跟踪（低负载 SOT 与级联重检测）

> 状态：In Progress（2026-09-21 立项生效：[DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md)
> / [DEC-020](../decisions/DEC-020-tracker-backend-spi.md) 经负责人批准转 Accepted）
> 负责人：linductor
> 所属计划：[Mirador 实施总计划](mirador-implementation-plan.md)（`SCOPE-13`）
> 前置：M6（已完成）；建议与 `DEC-018` 阶段 2 的真实数据评估协调排期，但不互为前置
> 建议发布点：`v0.4.0`（暂定，随判定收尾确认）
> 更新日期：2026-09-23

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
  修复后复验；release/asan/ubsan/warnings 预设的完整 ctest 未在该修复
  commit 之上重跑，六预设完整门禁随编排脚本在本提交之上重跑确认。
- CI 回填：待补（分支未推送；PR 与 CI run 链接随 CI 门禁落地回填，同
  M7-01/M7-02 先例）。
