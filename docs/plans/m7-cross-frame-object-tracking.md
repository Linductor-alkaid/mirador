# M7：跨帧目标跟踪（低负载 SOT 与级联重检测）

> 状态：In Progress（2026-09-21 立项生效：[DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md)
> / [DEC-020](../decisions/DEC-020-tracker-backend-spi.md) 经负责人批准转 Accepted）
> 负责人：linductor
> 所属计划：[Mirador 实施总计划](mirador-implementation-plan.md)（`SCOPE-13`）
> 前置：M6（已完成）；建议与 `DEC-018` 阶段 2 的真实数据评估协调排期，但不互为前置
> 建议发布点：`v0.4.0`（暂定，随判定收尾确认）
> 更新日期：2026-09-24

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
  变）；release/tsan/warnings 预设与六预设完整复跑随编排脚本收口（CI 矩
  阵无 release 预设，同 M7-03~06 先例）；滚动往返为合成口径（`DOD-05`
  不宣称真实场景效果），`min_compensation_confidence` 0.0 与
  `max_generation_lag` 1 为开发冒烟初值（`DEC-019` 第 5 条，M7-09 校准
  收口，`RISK-2026-17` 随 A/B/C/D 矩阵 B/C 差值与 `*-scroll` 延续率门
  控）；重检测原语与身份复核归 M7-08；既有遗留（非本项引入）：头文件
  声明的单参 `ObjectTracker::terminate(uint64_t)` 重载在 src 中无定义
  （object_tracker.hpp:594，冒烟曾触发链接错误），建议 M7-08 或后续
  refactor 清理。
- CI 回填：待补（分支未推送，推送与 14/14 证据回填随编排脚本收口）。
