# Mirador 跨帧目标跟踪（低负载 SOT）设计

> 状态：Active（随 [DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md)
> 于 2026-09-21 Accepted 生效；M7 内 Experimental 契约以 go/no-go 判定为冻结前置）
> 日期：2026-09-21
> 负责人：linductor
> 上位设计：[Mirador 开发设计方案](mirador-development-design.md)（§10、§11、§12、§16、§18、§24）
> 里程碑：M7（[docs/plans/m7-cross-frame-object-tracking.md](../plans/m7-cross-frame-object-tracking.md)）

## 1. 目的与范围

本文定义 Mirador 的跨帧目标跟踪能力：在 `PerceptionSession` 内对同一视觉对象跨帧
维持身份（stable ID）与位置估计，以近零常态成本运行，在对象丢失时以显式状态与
预算化原语支持上层触发高负载重检测（YOLO 类 Detector Backend）。它把设计文档 §16
的稳定 ID 从"相邻快照关联"扩展为带时间维度的持续跟踪，是 M6 遗留的
`temporal_stability` 跨帧工作的承载工作项。

调研结论（2026-09）支撑的三个设计前提：

1. **低负载持续跟踪可行**：终端/UI 场景目标刚性、静止占空比高、位移温和、几何
   结构丰富；通用视频跟踪最难的形变与遮挡问题在此场景基本不存在。业界对照：
   经典轻量 tracker（MOSSE ~450 FPS / KCF ~150 FPS / CSRT 25–50 FPS，OpenCV
   基准量级）与轻量深度 tracker（Ocean 0.6 MB、LightTrack 2.4 MB、NanoTrack
   Apple M1 CPU >200 FPS）证明各档成本可行，但 UI 场景下深度方案的成本
   （GFLOPs 级 + runtime 依赖）相对传统方案高 2–3 个数量级而收益有限。
2. **"轻量持续 + 丢失后高负载重检测"是长时跟踪标准范式**（TLD 一脉；VOT-LT
   评测共识：长时性能由丢失判定准确性与全图重检测能力决定），置信度判定有成熟
   方法（响应峰值旁瓣比 PSR、平均峰值相关能量 APCE），重检测触发与身份复核
   （检测候选 + 外观验证两段式）均为既有公开做法。
3. **位置-时间先验是有条件成立的软证据**（见第 3 节统计模型）：UI 场景高静止
   占空比使其似然比高，但滚动与布局突变会造成结构性失效，必须配运动补偿与
   布局代际。

### 非目标

- 不做多目标跟踪（MOT）关联算法；多目标由多 `TargetTrack` 并行 + 既有融合
  稳定 ID 承担，不引入匈牙利等全局最优指派（`DEC-010` 同款取舍）。
- 不在核心内置模型 runtime；轻量深度 tracker 经 `TrackerBackend` SPI 注入
  （[DEC-020](../decisions/DEC-020-tracker-backend-spi.md)），NanoTrack ncnn
  参考实现位于默认构建不获取的 `integrations/`（`DEC-015` 机制），权重不入仓。
- 不硬编码"何时调用重检测"的全局策略（`RULE-12`）；只提供原语、指标与默认
  示例策略。
- 不承诺跨会话、跨应用重启或跨设备的身份唯一（`RULE-09`）。

## 2. 能力分层

| 层 | 内容 | 依赖 | 状态 |
| --- | --- | --- | --- |
| A | 跟踪状态机、目标池、变化检测门控短路、邻域验证（模板 NCC + 闭合结构）、丢失判定、级联重检测原语 | 既有模块契约，零新依赖 | M7 交付 |
| B | 全局运动补偿、布局代际、位置-时间条件化证据 | image 模块新增全局位移估计原语（纯 CPU 确定性） | M7 交付 |
| C | CF tracker（KCF/MOSSE 类）进 geometry 可选实现 | 传统视觉实现，无模型依赖 | `POST-07` |
| D | `TrackerBackend` SPI + NanoTrack ncnn 参考后端（可选增强通道） | 公共契约扩展（`DEC-020`）+ `integrations/`（复用 `DEC-015` ncnn 基础设施） | M7-11~13 |

模块归属（依赖方向遵循 `DEC-013`，指向 Mirador 抽象）：

- `mirador::fusion`：跟踪状态机、目标池、证据融合、丢失判定、重检测策略原语。
  跟踪状态随 `PerceptionSession` 持有，会话内使用，无跨 session 共享。
- `mirador::image`：全局位移估计原语；验证 ROI 裁剪复用既有组件。
- `mirador::cache`：不新增契约。模板 NCC 匹配复用 M3 已交付的模板匹配组件；
  目标模板集为 fusion 内部有界结构，不扩张 `VisualIndex` 契约（其图标识别索引
  职责由 `DEC-014` 冻结）。
- `mirador::geometry`：闭合结构一致性证据经已冻结的
  `GeometricRegionProposal` 契约（`DEC-018` 阶段 1）在验证 ROI 内运行并消费其
  描述量（`closure_score`、`rectangularity`、`edge_support`）。该使用方式不进入
  设计 §8 输出模型、不作为 §16 融合证据源，因此不依赖 `DEC-018` 阶段 2。

## 3. 位置-时间证据的统计模型

跟踪身份判定采用显式证据融合，不伪装概率（同 `DEC-010` 风格）。位置先验的
合法形式是似然比：

```text
P(同一对象 | 候选落入邻域) ∝ P(落入邻域 | 同一对象) · P(同一对象) / P(落入邻域 | impostor)
```

UI 场景的定性性质与失效条件均已识别，证据权重必须按运动状态条件化：

| 运动状态 | 先验有效性 | 依据 | 处置 |
| --- | --- | --- | --- |
| 静止期（占空比大头） | 强成立 | 目标位置以接近 1 的概率不变；impostor 仅剩新弹出/动画经过两种低基率来源 | 位置门控全权重 |
| 滚动/窗口拖动 | 未补偿时似然比**反转**（目标移走、下方元素滚入旧位置），结构性失效，调阈值不可救 | 全局一致位移场 | 全局运动补偿后恢复静止期性质（第 6.3 节） |
| 布局突变（弹窗/切页/主题切换） | 历史位置分布整体失效 | 位置分布突变 | 布局代际递增，位置先验清零降权，模板/语义证据跨代保留（第 6.4 节） |

另一结构性失效单列为 swap 风险：同画面同类元素静止于相邻位置时，位置门控会把
对方门进来。处置为 impostor 负证据（第 5 节目标池负模板），对应文献
distractor-aware 机制。`RISK-2026-16` 记录该风险。

证据强度分级（决定状态机转移，不决定身份宣称）：

- **强**：外观证据高置信（模板 NCC 峰值与峰旁瓣质量同时达标，或闭合结构一致）
  + 位置门控内 + 语义兼容 → 可确认延续。
- **中**：外观弱匹配 + 位置门控内 + 语义兼容 → 临时延续，标记低置信
  （generation 语义天然允许上层拒绝）。
- **弱**：仅位置先验（外观缺失）→ 只维持运动外推占位框，不宣称同一对象，
  状态转 `kUncertain` 等待下一次验证。
- **否决**：位置门控内但语义冲突（类别/标签谓词不兼容，同 `DEC-010` 门控语义）
  或命中 impostor 负模板 → 排除该候选。

## 4. 跟踪状态机

```text
            确认延续                连续 N 帧 外观证据不足
  kTracking ────────→ kTracking   kTracking ──────→ kUncertain
      ↑                  ↑  │                        │
      │ 重捕获(身份复核   │  │ 外观证据恢复           │ 证据枯竭/超时
      │ 通过)            │  └────────────────┘       ↓
      │                  │                        kLost ────→ kTerminated
      └──────────────────┴───────────── 重检测候选复核通过      重试预算耗尽/上层终止
```

- `kTracking`：正常跟踪。变化检测未命中目标区域时近零成本维持（第 6.1 节）。
- `kUncertain`：位置有估计但外观证据不足；输出占位框并携带低置信标记；
  持续 `uncertain_frame_limit` 帧未恢复转 `kLost`。
- `kLost`：目标消失。记录丢失时刻与最后位置；是否触发重检测由上层依据
  Mirador 输出的建议指标决定（第 7 节）。
- `kTerminated`：重试预算耗尽或上层显式终止；track 归档，ID 不再延续。
  失败对调用方可见，不静默清池。

状态转移全程确定性：同输入序列产生同状态序列，无墙钟依赖（同 `DEC-010`
generation 规则口径）。

## 5. 目标池数据模型

```cpp
// 契约示意，最终形态以 M7-01 冻结为准（Experimental 直至 M7-10 判定）
struct TargetTrack {
    uint64_t track_id;                 // 延续融合 stable_id（RULE-09）
    TrackState state;
    RectF last_bounds;                 // 最后确认/外推位置，帧族坐标空间
    PointF predicted_center;           // 运动模型外推
    std::vector<TrackObservation> position_history;  // 有界，按布局代际分组
    uint32_t layout_generation;        // 目标存续期间经历的最大布局代际
    TemplateSet templates;             // 有界多模板：初始 + 高置信更新外观
    TemplateSet negative_templates;    // 有界：本代际已确认的同类 impostor
    SemanticLabels labels;             // 类别/标签（Detector/调用方附加）
    float confidence;
    uint64_t last_verified_sequence;
};
```

资源约束（`RULE-06`）：目标数、每 track 位置历史条数、模板数、负模板数与总
字节预算全部显式配置且默认值冻结于 M7-01；超限为显式淘汰（LRU/最旧优先）并
计入 trace，不得无界增长。目标池为小型结构化数据 + 灰度 patch 模板，常态内存
占用在 KB 量级，随目标池字节预算报告。

M7-01 冻结落点：`include/mirador/object_tracker.hpp`（`ObjectTracker` 有界
目标池；本图的 `SemanticLabels` 冻结为 `TrackSemantics` 的 `label`/`text`
两个有界字段，其余字段一一对应；帧级管线方法随 M7-03 起在同一 Experimental
头内扩展）。

## 6. 跟踪管线

### 6.1 变化检测门控三级短路

复用设计 §10/§11 按需执行管线，跟踪主循环按成本递增短路：

1. 画面未显著变化 → 全部 track 位置直接复用，成本 ≈ 指纹比较（近零路径）。
2. 变化 ROI 与某 track 外推位置不相交 → 该 track 短路复用。
3. 变化 ROI 与外推位置相交 → 该 track 进入邻域验证（6.2）。

静止占空比高的 UI 场景使平均成本趋近于零路径，这是本能力区别于通用视频
跟踪（每帧必跑）的成本结构基础。

M7-03 冻结落点：门控入口为 `ObjectTracker::evaluate_change_gate`
（Experimental，`object_tracker.hpp`）——消费调用方跑出的 `ChangeReport`
（tracker 不自持上一帧、不重复实现变化检测），输出按 track_id 升序的逐
track 决策（复用/待验证/非活跃）。门控是纯决策：不改池内任何状态，短路
复用不推进 `last_verified_sequence`（位置先验非外观证据，设计第 3 节），
证据级确认随 M7-06 状态机、历史簿记经 `record_observation` 留给调用方；
非 `kTracking` 态显式返回决策值不静默跳过；`kGlobal` 触发的代际递增判定
归 M7-07。运动补偿（6.3）落地前，滚动类变化与全部 track 相交、全员进入
验证入口是本阶段的预期行为（`RISK-2026-17`）。

### 6.2 邻域验证

在预测位置 ± 门控半径的验证 ROI 内执行双通道外观验证：

- **E1 模板 NCC**：多模板取最优；置信度综合峰值与峰旁瓣质量（PSR 思想的
  NCC 峰/邻域均值比），抑制平坦响应误判。
- **E2 闭合结构一致性**：验证 ROI 内运行 `GeometricRegionProposal`，当前
  `closure_score`/`rectangularity`/`edge_support` 与池内基线的偏差在容差内。
  该通道对主题切换、内部文字变化、部分遮挡鲁棒，是灰度模板（E1）失效时的
  主证据。

E1 与 E2 按显式规则组合（第 3 节分级），阈值初值冻结于 M7-01，校准于 M7-09。

**增强通道（可选注入）**：调用方注入 `TrackerBackend`（`DEC-020`，如 NanoTrack
参考后端）时，邻域验证追加深度外观证据：以 track 当前 bounds 调用 update 获得
bbox 与置信度，作为 E1 的形变鲁棒替代/复核信号——参与条件为上层启用开关
（`RULE-12`），典型场景为形变/遮挡密集输入与快速位移（E1 搜索窗跟不住）。
组合规则保持显式确定性：深度通道与 E1 同位一致时互证升级置信，冲突时按
保守侧处置（降级 `kUncertain`，`RISK-2026-18`）；未注入 `TrackerBackend` 时
管线零变化，优雅退化为纯传统双通道。高置信模板更新仅取自双通道一致帧，
防止深度 tracker 漂移污染模板池。

M7-05 冻结落点：邻域验证器交付于 `ObjectTracker::verify_track`（Experimental，
`object_tracker.hpp`）与配套类型 `TrackStructureDescriptors`/
`AppearanceVerification`/`StructureVerification`/`TrackVerification`。验证器
为纯逐 track 决策（`const`，任何路径不改池状态、不推进
`last_verified_sequence`，分级→状态转移与代际判定归 M7-06，同 M7-03 门控
先例）；接受任意非 `kTerminated` 态 track（M7-08 重检测身份复核复用同一
入口）。四项契约裁决冻结于头注释：

- **E1**：验证 ROI 内"平移窗口完全落在 ROI 内"的整数平移全集，逐候选以
  `adopt_track` 同款管线（`crop` + M3-09 `make_visual_patch_fingerprint`，
  `template_thumb_side`）提取 patch，与池内全部正模板做与 VisualIndex 模板
  层（M3-10）同一归一化的 NCC；逐模板响应面取峰（总序：峰值 → 切比雪夫
  半径 → dy → dx），PSR = (峰 − 旁瓣均值)/(旁瓣总体标准差 + 1e-12)（平坦
  响应面得 0，单候选搜索集得峰/1e-12）；胜者模板取总序（峰值 → PSR →
  模板序）。kStrong 要求峰值 ≥ `ncc_strong_threshold` 且 PSR ≥
  `peak_sidelobe_ratio_min` 同时达标；PSR 不达标时峰高一律不采信（kNone）。
  贴边 track 的 ROI 钳制后小于窗口时退化为仅评估 (0, 0) 偏移（"还在原位
  吗"检查，`best_offset_* == 0` 可见）。负模板只读边界：验证器不读
  `negative_templates`，impostor 采集与 `kVetoed` 否决归 M7-06。
- **E2**：消费裸描述量 `TrackStructureDescriptors`
  （closure_score/rectangularity/edge_support，[0, 1] 校验）而非
  `GeometricRegionProposal` 类型——保持 `mirador_fusion` 链接接口恰为
  core/image/cache，零新依赖（`DEC-019` 第 1 条阶段 A 口径，架构测试与
  链接闭包探针不变）；调用方经 `ObjectTracker::verification_roi`（与验证器
  同一 ROI 规则的纯查询）在验证 ROI 内运行线段检测 + `propose_regions`
  后传入。基线经显式簿记方法 `record_structure_baseline` 入池：每 track
  单槽、后写覆盖（漂移控制留调用方/M7-06 策略），占
  `kStructureBaselineOverheadBytes` 计入 `byte_size` 与 `pool_budget_bytes`
  （放不下显式 `kBudgetExceeded`），`terminate`/track 淘汰/`reset` 释放；
  无基线时 E2 显式报 `kNoBaseline` 不伪造结论。偏差口径：
  |q − 基线_q| / max(|基线_q|, 1e-6) 取三量最大值 ≤
  `structure_deviation_tolerance` 为一致（偏差不截断上报，供 M7-09 校准）。
- **验证 ROI**：`last_bounds` 以 `verification_roi_diagonal_ratio × 对角/2`
  每侧围绕 `predicted_center` 扩展（M7-07 前 `predicted_center` 恒为
  `last_bounds` 中心），按 adopt_track 覆盖规则取整并钳制到视图。
- **预算与取消**：E1 扫描规划工作量（逐候选 2×窗口字节 + 缩略图字节 +
  2×模板数×缩略图字节 + 响应面存储，饱和算术）先于任何像素读取对比
  `verification_work_budget_bytes`（初值 256 MiB 开发冒烟值），超限显式
  `kBudgetExceeded`；取消/超时经 `ExecutionContext`（入口 + 逐行检查，
  校验先于取消，不返回半份结果）。同一输入逐位确定（固定扫描序 + 上述
  总序 + 精确整数和上的 double 单次除法）。

### 6.3 全局运动补偿

`mirador::image` 新增全局位移估计原语：基于既有分块差分的低分辨率平移搜索
（纯 CPU、确定性、预算内），输出全局位移向量与置信度。变化检测判定为全局
一致位移时，对池内全部 track 施加同一位移校正并更新速度估计；补偿后位置
先验恢复静止期有效性（第 3 节）。补偿量参与坐标变换链，遵守 `RULE-05`
（`Transform2D` 组合、方向/往返测试矩阵适用）。

M7-04 冻结落点：原语本体交付于 `include/mirador/shift_estimation.hpp`
（Experimental，随 M7 go/no-go 判定冻结）——`estimate_global_shift` 双入口
（`ImageView` 双帧 / M1 `ChangeSignature` 双签名；签名版与视图版在相同
缩略图尺寸下逐位一致）。搜索语义：两帧经与变化检测相同的确定性管线
（整数 area 重采样 + BT.601 luma）缩至方形灰度缩略图，在
`[-max_shift, max_shift]²` 整数平移全集上以固定中心比较窗（每候选等像素
数）做 SAD 全遍历；胜者取总序 (SAD, 切比雪夫半径, dy, dx) 最小者，纯整数
运算。语义决策冻结于头注释：置信度为缩略图 SAD 表面的峰显著度
`(μ_others − best) / (μ_others + best)` ∈ [0, 1]（整数和 + 单次 double
除法；单候选/全平手为 0，缩略图分辨率逐像素相同的帧对为 1）；位移精度为
精确有理数 `thumbnail_shift × frame_dim / thumbnail_size`（double 求值后
收窄 float），方向为 p_curr = p_prev + (dx, dy)，经 `make_translation`
进入 `Transform2D` 组合链（`RULE-05`，DOD-03 矩阵适用）；默认参数
（thumbnail 64 / max_shift 16 / 预算 512 KiB）为开发冒烟值，M7-09 校准。
两帧必须同呈现尺寸；比较恒在呈现像素上进行（同 `detect_change` 约定）。
搜索循环为有界非平凡循环，经 `ExecutionContext` 入口 + 逐行检查显式转化
kCancelled/kTimeout，不返回半份结果。消费侧联动（对池内 track 施加位移
校正、`advance_layout_generation` 判定）仍归 M7-07，本原语不触碰
`ObjectTracker` 状态。

### 6.4 布局代际

布局代际（`layout_generation`）由变化检测的全局变化分类（§11 已有全局/局部
输出）递增：弹窗、切页、主题切换等全局事件使代际 +1。代际语义：

- 位置历史按代际分组存取；代际切换后位置先验权重清零，`kTracking` 中目标
  降级为 `kUncertain` 直至外观证据重新确认。
- 模板与语义标签跨代保留（外观证据不受布局突变影响；主题切换导致 E1 失效
  时由 E2 承接）。
- track 的 `layout_generation` 落后当前全局代际超过阈值且证据枯竭 → `kLost`。

### 6.5 融合层对接

跟踪输出以带状态的区域集合进入既有融合与稳定 ID 流程。`StableIdTracker`
（`DEC-010`）的门控谓词在跟踪会话内扩展时间一致性信号：跟踪确认的
track ↔ 区域配对在门控阶段直接放行（成本置优），未跟踪区域走既有
IoU/包含门控。该扩展属于 `DEC-010` 第 4 节预留的"阈值配置演进"通道的跨帧
推广，不破坏已冻结契约；`DEC-010` 当时将中心距离降为 trace 观测量是静态
融合场景的正确取舍，跨帧场景的时间连续性赋予了空间邻近身份含义，两者的
统计前提不同，以本决策记录为界。

## 7. 级联重检测（丢失后高负载路径)

`kLost` 后的重检测遵循长时跟踪标准范式（触发 → 全图粗召回 → 身份复核 →
ID 语义），在 Mirador 中映射为既有按需 Detector 路径的策略化：

- **触发与节流原语**（fusion 提供，上层决策，`RULE-12`）：建议参数含退避
  序列（连续失败指数退避）、最大重试次数、变化门控联动（静止画面不重试——
  与 6.1 同一近零门控适用于重检测本身）与预算耗尽的显式失败上报。
- **粗召回**：上层调用 Detector Backend（YOLO 类）全屏或局部扫描；
  Mirador 提供按目标语义标签过滤候选的通用组件。
- **身份复核**：候选 ROI 内以池内多模板 + E2 通道复核（复用 VisualIndex
  模板 NCC 组件），防止同类干扰物误认——文献"检测候选 + 外观验证"两段式。
- **ID 语义**：复核通过 → 延续 `track_id`（记录中断事件）；外观与池内证据
  不足以确认 → 按融合规则分配新 ID 并记录关联；预算耗尽 → `kTerminated`，
  失败可见。

成本结构：常态路径近零；Detector 调用仅在丢失触发，频率受退避与变化门控
约束，作为一等指标上报（设计 §20"每分钟 Backend 调用次数"口径）。

## 8. 测试与验证

指标（合成 harness 先行，真实数据评估为转正前置，沿用
[evaluation-scenes](../benchmarks/evaluation-scenes.md) 离线接入约定）：

| 指标 | 口径 |
| --- | --- |
| ID 延续正确率 | 对真值的 track 延续正确比例，静止期与补偿后滚动分开列报 |
| swap 率 | 同画面同类对象身份互换次数（`*-similar-icons` 压力场景） |
| 假阳性延续率 | impostor 被确认为目标的错误延续 |
| 丢失误判率 | 双向：目标在判丢 / 目标失未判丢 |
| 重捕获成功率与延迟 | 触发重检测后正确恢复的比例与帧数 |
| 验证路径 p50/p95 | 各级短路路径与邻域验证路径耗时 |
| Detector 触发频率 | 每分钟高负载调用次数（预算合规口径） |
| 目标池内存 | 峰值 RSS 贡献与字节预算余量 |

基准方法对比矩阵（M7-09 发布，隔离各证据通道贡献）：

- A 仅外观（E1+E2）；B 外观+位置门控（无补偿）；C 外观+位置门控（全局
  运动补偿）；D 外观+位置+语义。

场景复用：`*-static-page`（近零路径与静止期延续）、`*-scroll`（补偿与滚动期
先验）、`*-dialog`（代际切换与重检测）、`*-theme-switch`（E2 承接 E1 失效）、
`*-similar-icons`（swap 与负证据）、`*-partial-anim`（低置信占位）。真实截图
评估与 `DEC-018` 阶段 2 共享同一批 `~/mirador-eval/` 数据采集，一次采集两用。

测试矩阵：契约确定性（同输入同输出）、预算淘汰显式性、状态机边界
（`uncertain_frame_limit`、代际切换、`kTerminated` 失败可见）、补偿坐标
方向/奇数尺寸/往返容差（`DOD-03` 矩阵）、属性测试（任意合法变换无越界）、
负向测试（超预算淘汰、语义冲突否决、静止画面重检测不触发）、性能基线
（`DOD-05`，数字随 M7-09 发布于 `docs/benchmarks/`）。

## 9. 隐私与资源

沿用 `RULE-10`：目标模板为内存中的灰度 patch，不落盘、不联网、不入日志；
trace 默认关闭或采样，不输出模板内容与原始帧。目标池字节预算计入
`PerceptionSession` 资源报告。

## 10. 演进项

- `POST-06` → 已升级为计划性交付（2026-09-21 按负责人指示）：`TrackerBackend`
  SPI 契约见 [DEC-020](../decisions/DEC-020-tracker-backend-spi.md)，NanoTrack
  ncnn 参考后端随 M7-11~13 交付于 `integrations/`（复用 `DEC-015` 的
  `NcnnRuntime` 与 `MIRADOR_BUILD_INTEGRATIONS` 默认 OFF 机制；引入前完成
  许可证与模型来源审查并登记 `docs/supply-chain/`）。其余轻量深度 tracker
  （LightTrack/Ocean 等）若后续引入，同走该 SPI，不新增契约面。
- `POST-07` CF tracker（KCF/MOSSE 类）作为 geometry 可选传统实现：触发——
  邻域验证在快速位移场景精度不足且模板 NCC 与深度增强通道均不可救。无模型
  依赖，引入方式同 `DEC-009`（源码引入先许可证审查）。
- 与 `DEC-018` 阶段 2 的衔接：阶段 2 立项后，`temporal_stability` 正式契约
  与本设计的 E2 通道共享基线数据；两者结论互不阻塞（M7 使用已冻结契约的
  每帧描述量，不依赖阶段 2 的跨帧契约）。
