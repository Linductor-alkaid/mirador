# DEC-020：TrackerBackend SPI 契约（有状态跟踪后端）

> 状态：Proposed（待负责人评审批准；最迟 M7-11 实施前冻结）
> 日期：2026-09-21
> 负责人：linductor
> 冻结里程碑：M7
> 替代/被替代：无（承接 [DEC-019](DEC-019-cross-frame-object-tracking.md)
> 决策第 6 条，扩展 [DEC-012](DEC-012-backend-spi-contract.md) 的 SPI 体系）
> 关联：[跟踪设计](../design/object-tracking-design.md)、
> [DEC-002](DEC-002-no-model-runtime-in-core.md)、
> [DEC-003](DEC-003-no-opencv-in-public-api.md)、
> [DEC-015](DEC-015-reference-runtime-selection.md)

## 背景与问题

[DEC-019](DEC-019-cross-frame-object-tracking.md) 决策 6 将轻量深度 tracker
（NanoTrack 等）定为 M7 计划性交付的可选增强通道。深度 tracker 需要模型
runtime（ncnn），按 `DEC-002` 必须经 Backend SPI 注入。但既有
`OcrBackend`/`DetectorBackend`（`DEC-012`）的契约语义是**无状态单次执行**：
相同输入必须产出相同结果，以此支撑能力结果缓存。跟踪后端本质不同——

1. 模板/滤波状态在 backend 内部跨调用存活（初始化于首帧，逐帧更新）；
2. "相同输入相同结果"不变量不成立（update 结果依赖历史帧序列）；
3. 一个 backend 实现需要同时服务多个被跟踪对象（多 `TargetTrack`）；
4. 失败恢复语义不同：状态损坏时必须显式报错并由 fusion 侧降级，而非返回
   旧结果。

需要决策的是：`TrackerBackend` SPI 的生命周期形态、状态归属、同步边界与
缓存语义，使其既容纳 NanoTrack 类实现，又不破坏 `DEC-012` 已冻结的两个
SPI 与 `RULE-03`/`RULE-07` 语义。

## 决策

1. **生命周期形态：会话句柄（handle）制**。`TrackerBackend` 提供三段式
   同步接口：

   ```cpp
   class TrackerBackend {
   public:
       virtual ~TrackerBackend() = default;
       virtual BackendInfo info() const = 0;

       // 初始化一个跟踪会话：模板在 backend 内建立。返回独立会话句柄，
       // 一个 backend 实现可并发持有多个句柄（是否线程安全由 info().thread_safe
       // 显式声明；同一句柄不允许并发调用）。
       virtual Result<std::unique_ptr<TrackerSession>> initialize(
           const ImageView& prepared_image,
           const TrackerInitRequest& request,
           const ExecutionContext& context) = 0;
   };

   class TrackerSession {
   public:
       virtual ~TrackerSession() = default;
       // 单帧推进：返回新 bounds 与置信度。状态在会话内更新。
       virtual Result<TrackerUpdateResult> update(
           const ImageView& prepared_image,
           const TrackerUpdateRequest& request,
           const ExecutionContext& context) = 0;
   };
   ```

   （形态示意；字段与错误语义在 M7-11 契约冻结时定稿，本决策冻结的是
   下列语义边界。）

2. **状态归属与生命周期**：跟踪状态归 backend 会话，`TrackerSession` 的
   析构即显式丢弃状态；fusion 侧 `TargetTrack` 持有会话句柄，track 终止
   （`kTerminated`/目标池淘汰）时同步析构。backend 执行失败或状态损坏必须
   显式报 `Status`（含 `kBackendFailed` 语义），由 fusion 侧将对应 track
   降级 `kUncertain` 并可重建会话——失败可见，不返回陈旧结果。

3. **同步边界**（`RULE-03` 适用不豁免）：`initialize`/`update` 均为同步
   调用，以 `const ExecutionContext&` 接收取消与 deadline，实现必须显式报
   `kCancelled`/`kTimeout`；backend 内部不得创建线程、线程池或定时器，
   ncnn 等 runtime 的调用线程由适配层自己的调用方线程承担。多句柄并发性
   由 `BackendInfo.thread_safe` 显式声明，单句柄内严格串行。

4. **缓存语义**：跟踪会话状态**不进入**能力结果缓存——`RULE-07` 的缓存键
   不变量（相同输入相同结果）对有状态 update 不成立，缓存层不得对
   `TrackerBackend` 调用做命中/复用。可缓存的仅有 `initialize` 的确定性
   前处理产物（如模板张量准备），键含 Backend 名称、实现版本、模型 ID/
   修订与请求参数摘要（`RULE-07` 键构成适用）。

5. **坐标与输入语义**（`DEC-012` 同款）：Backend 输出落在
   `prepared_image` 像素空间，坐标恢复由 Mirador 依预处理链逆变换完成；
   输入格式经 `BackendInfo.accepted_formats` 声明；算法不得静默修改输入。

6. **实现身份与依赖边界**：`BackendInfo` 沿用 `DEC-012` 字段
   （name/implementation_version/model_id/model_revision），用于诊断、
   评测归因与供应链登记。参考实现（NanoTrack ncnn）位于 `integrations/`，
   默认构建零获取（`DEC-015` 机制）；`mirador-core` 仅含上述纯虚接口，
   链接闭包不变（架构测试口径不变）。模型权重不入仓，示例经用户显式
   路径提供（工程规范 9.2.4）。

## 备选方案

- **无句柄、backend 内部维护单一 track 状态**：多目标场景需每目标一个
  backend 实例，所有权与生命周期管理移给调用方且易错，句柄制更清晰，被否。
- **将跟踪编排放 core、backend 只做"单帧模板匹配"无状态调用**：深度
  tracker 的跨帧状态（模板更新、尺度滤波）无法拆成无状态单次执行，强拆
  会使契约失真并把 runtime 语义泄进 core，被否。
- **复用 `DetectorBackend` 表达跟踪**（每帧检测 + 关联）：语义错位——
  检测无目标身份概念，且 NanoTrack 类单目标 tracker 的成本结构完全不同，
  被否。
- **不新增 SPI、由 fusion 直接依赖 ncnn**：直接违反 `DEC-002`，被否。

## 影响与风险

- 这是既有 SPI 体系首次引入**有状态**契约语义：`DEC-012` 的"相同输入必须
  产出相同结果，以支撑能力结果缓存语义"条款显式不适用于
  `TrackerSession::update`，需在 `DEC-012` 关联文档与 API 索引中以注记
  界定适用范围（不改其原文语义）。
- `RISK-2026-18`（深度通道置信冲突/漂移污染）由 fusion 侧组合规则门控：
  冲突保守降级、高置信模板更新仅取双通道一致帧、上层可整体关闭注入。
- 公共契约面 +1（两个新头文件级别的接口）： Experimental 至 M7-10 判定
  后经决策冻结，同 `ObjectTracker` 口径。
- NanoTrack 参考实现的许可证与模型来源审查是 M7-12 前置项，审查未通过则
  参考后端更换候选（LightTrack/Ocean ncnn 移植同走本 SPI，契约不变）。

## 验证方式

- M7-11：契约头文件 + Fake `TrackerBackend`（固定轨迹注入）完成 fusion
  侧编排测试（初始化/更新/失败降级/句柄析构/取消 deadline 语义）；
  架构测试证明 core 链接闭包不变。
- M7-12：NanoTrack ncnn 参考后端合成模型冒烟（`integrations` 套件）+
  `docs/supply-chain/` 许可证与来源登记 + CI job（默认构建零获取口径）。
- M7-13：深度通道组合规则、未注入退化路径、置信冲突保守处置的单测与
  属性测试。
- 全程：6 预设 + sanitizer + lint 双口径（`integrations` 面随
  `MIRADOR_BUILD_INTEGRATIONS=ON` 口径）。

## 关联文档和工作项

- [DEC-019](DEC-019-cross-frame-object-tracking.md)（能力立项与增强通道定位）
- [DEC-012](DEC-012-backend-spi-contract.md)（SPI 体系与无状态语义的适用
  范围界定）、[DEC-015](DEC-015-reference-runtime-selection.md)（ncnn 基础
  设施复用）
- [跟踪设计](../design/object-tracking-design.md) §2/§6.2（增强通道）
- [M7 里程碑](../plans/m7-cross-frame-object-tracking.md) `M7-11`/`M7-12`/
  `M7-13`
- 总计划 `SCOPE-13`、`POST-06`（升级为计划性交付）、`RISK-2026-18`
