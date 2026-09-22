# 更新日志

本文件格式参考 Keep a Changelog，版本号遵循语义化版本；每个里程碑的发布点与
[实施总计划](docs/plans/mirador-implementation-plan.md)的里程碑索引一一对应。

## [Unreleased]

### 新增（M7 跨帧目标跟踪，`DEC-019`，Experimental）

- M7 立项生效（2026-09-21，负责人批准）：[DEC-019](docs/decisions/DEC-019-cross-frame-object-tracking.md)
  （跨帧目标跟踪能力立项）与 [DEC-020](docs/decisions/DEC-020-tracker-backend-spi.md)
  （`TrackerBackend` SPI 契约）转 Accepted，[跟踪设计](docs/design/object-tracking-design.md)
  与 [M7 里程碑](docs/plans/m7-cross-frame-object-tracking.md)生效。
- M7-01：`mirador::fusion` 新增 Experimental 公共契约
  `object_tracker.hpp`——`TrackState` 四态生命周期（`kTracking/kUncertain/
  kLost/kTerminated`）、`TargetTrack` 有界目标池数据模型（位置历史/模板/负模板/
  语义快照）与 `ObjectTracker` 池管理（`create` 选项校验、`adopt_track`
  模板捕获与显式淘汰、`terminate` 失败可见、预算与淘汰 trace 计数）。全部
  默认值（目标数、位置历史上限、模板/负模板数、字节预算、
  `uncertain_frame_limit`、验证阈值初值、重检测退避初值）冻结于
  `ObjectTrackerOptions`，阈值初值随 M7-09 校准。帧级跟踪管线（门控短路/
  邻域验证/运动补偿/级联重检测）随 M7-03 起在同一 Experimental 头内扩展；
  契约经 M7-09/M7-10 go/no-go 判定后才计入兼容性承诺。
- M7-02：`ObjectTracker` 新增池有界变更原语——`record_observation`（有界位置
  历史追加，溢出显式淘汰最旧并计入 `evicted_observation_count`，纯簿记不触碰
  身份/证据字段）、`add_template`（模板集存储，钉死初始模板、溢出淘汰最旧
  非初始模板）、`add_negative_template`（负模板存储，容量 0 显式拒绝）、
  `advance_layout_generation`（布局代际确定性递增）与代际分组查询
  `observations_in_generation`。全部路径维持字节预算与显式淘汰/显式错误语义
  （`RULE-06`：不静默增长、不静默丢弃）。

## [0.3.0] - 2026-09-20

M6「几何区域 Proposal 实验（实验轨道，`DEC-017`）」全部工作项落地与
Ubuntu 20.04（focal）适配门禁
（[PR #16](https://github.com/Linductor-alkaid/mirador/pull/16)、
[PR #17](https://github.com/Linductor-alkaid/mirador/pull/17) 及收尾
PR，CI 14/14 全绿）。go/no-go 判定为合成口径 GO；
[DEC-018](docs/decisions/DEC-018-geometric-region-proposal-promotion.md)
经负责人授权批准（Accepted），阶段 1 契约冻结生效。全部验证由
Independent-Verification-Agent 独立执行。

### 平台支持

- 新增 Ubuntu 20.04（focal）适配门禁：CI 以 `ubuntu:20.04` 容器 + 发行版
  `g++-10`（libstdc++ 10，`std::span` 下限）+ CMake 3.16.3 + Ninja 1.10 执行
  完整构建与测试。公开工具链下限：GCC/Clang ≥ 10（libstdc++ ≥ 10）、CMake
  ≥ 3.16；focal 自带 GCC 9.4 无 `std::span`，不受支持。GitHub 托管的
  ubuntu-20.04 runner 已退役，门禁改用 focal 容器承载，并把 focal 源可能
  迁至 old-releases 的 EOL 情况内置为自动兜底。运行验证随对应 PR 的 CI
  回填至[兼容性登记](docs/compatibility/compatibility.md)。

### 新增（M6 实验轨道，`DEC-017`）

- `mirador::geometry` 实验公共契约 `geometric_proposal.hpp`：`propose_regions`
  把线段组织为闭合/近闭合结构并输出 `GeometricRegionProposal`（closure/
  rectangularity/edge_support 评分、OMBR、Tight/Context 双 ROI）。纯 CPU、
  确定性、显式预算（`kBudgetExceeded`）与取消/deadline 支持。M6 内以
  Experimental 标记交付（issue #11、设计 §24 M6）；**本版本内经 `DEC-018`
  阶段 1 转正为正式契约**（见下方变更）。
- M6-04：合成验证 harness `mirador_bench_geometric_proposal`（五类场景 ×
  双口径）与首份数字发布（[linux-x64-geometric-proposal-2026-09](docs/benchmarks/linux-x64-geometric-proposal-2026-09.md)）：
  Mode A（精确线段 → 闭合分析层）汇总 recall 1.000 / precision 0.875 / 重复
  1 个/实体 / tight-ROI 缩减 ≥ 0.638，`DEC-017` 四项晋升门槛初值全部 PASS；
  Mode B（一方检测器端到端）按 `RISK-2026-09` 单独列报输入线段质量口径
  （圆角硬边光栅化碎裂为已知机制）；耗时随线段数曲线与 temporal 占位随页
  发布。真实截图离线接入约定补充进
  [evaluation-scenes](docs/benchmarks/evaluation-scenes.md)（数据不入仓）。
- M6-05（条件触发：M6-04 门槛达标）：跨帧稳定性探针与缓存增益测量
  （[linux-x64-geometric-proposal-reuse-2026-09](docs/benchmarks/linux-x64-geometric-proposal-reuse-2026-09.md)）：
  ±2 px 独立抖动 6 帧序列上 proposal 关联率 1.000、配对 IoU ≥ 0.979；
  VisualIndex 命中对照显示视觉歧义场景基线 top-1 0.250 → 几何门控 0.625
  （wrong 12→6），视觉可分场景门控零代价（1.000→1.000），并如实报告门控
  边界：几何相近的相邻尺寸实体（双边比落在窗口内）无法由几何区分。纯测量
  入口 `mirador_bench_proposal_reuse`，无核心与契约改动（`DEC-017` 第 6 条：
  增益只测量不作门槛）。
- M6-06（收尾判定）：go/no-go 判定为**合成口径 GO**——`DEC-017` 四项晋升
  门槛初值全 PASS（recall 1.000 / precision 0.875 / 重复 1 个/实体 /
  tight-ROI 缩减 min 0.638），完整判定记录与口径限定（仅合成证据
  `RISK-2026-14`、`RISK-2026-09` 输入质量口径、`DEC-011` 环境口径）见
  [M6 里程碑](docs/plans/m6-geometric-region-proposal-experiment.md)。
  转正决策 [DEC-018](docs/decisions/DEC-018-geometric-region-proposal-promotion.md)
  立档并经负责人授权批准（见下方变更）。

### 变更

- `DEC-018` 阶段 1 转正（经用户授权批准，Accepted 2026-09-20）：
  `geometric_proposal.hpp` 撤销 Experimental 标记，
  `docs/api/README.md` 与 `docs/compatibility/` 转为正式登记，**自本版本起
  计入兼容性承诺**；阶段 2（`temporal_stability` 契约与融合/输出模型集成）
  以真实截图评估为前置，另行立项，本版本不包含。

## [0.2.0] - 2026-09-15

M5「平台适配与产品化基准」全部工作项落地：POST-05 参考能力后端（`DEC-015`）、
平台采集适配示例与 `kDisplay` 变换契约（`DEC-016`）、产品化基准发布（`DEC-011`）、
并发/模糊/隐私验证矩阵与文档收口
（[PR #12](https://github.com/Linductor-alkaid/mirador/pull/12)，CI 13/13 全绿；
tag 待负责人授权）。全部测试由 Independent-Verification-Agent 独立编写与执行。

### 新增

- M5-02/03/04（`integrations/`，`MIRADOR_BUILD_INTEGRATIONS` 默认 OFF，默认构建
  零获取）：pinned ncnn（20260526）`NcnnRuntime` 包装；PP-OCR 参考后端（det 复用
  M3 DB 后处理、rec 贪心 CTC 解码、组合管线）；YOLO 系参考检测后端（YOLOv5 单
  张量输出契约、letterbox/NMS 复用、显式候选预算）。合成模型冒烟无权重可运行；
  真实权重评测按 `RISK-2026-13` 记录补跑条件。
- M5-05：可选采集适配——`adapters/capture-linux`（X11 窗口/整屏 → RGB8 `Frame`，
  Xvfb/XWayland 冒烟；XWayland root 无像素后备为文档化失败语义）、
  `adapters/capture-windows`（GDI BitBlt，CI 编译验证）、`adapters/capture-android`
  （MediaProjection AImageReader + Accessibility 转换，宿主测试 + NDK 编译验证）。
- M5-05（`DEC-016`）：`FusionOptions::display_transform`（kOriented→kDisplay，适配
  层提供）——`EvidenceSet` 与融合目标空间开放 kDisplay，kDisplay 参与时变换必备；
  `run_ocr`/`run_detector` 输出空间保持帧族（刻意收窄）。
- M5-06（`SCOPE-08`）：`benchmarks/measure_sizes.sh` 体积入口；`docs/benchmarks/`
  发布 Linux x64 数字（变化检测 unchanged p50 1.64 ms、缓存命中路径、模块静态库
  合计 0.61 MiB）；评测集场景清单与离线数据接入约定（数据不入仓）。
- M5-07：并发矩阵（已发布快照跨代并发读、会话级并行，TSAN 常规运行）；模糊入口
  `tests/fuzz/`（DB 后处理、缓存键、坐标变换，`MIRADOR_BUILD_FUZZ` clang-only）
  与 CI 限时 fuzz job；隐私负向测试（管线零落盘、Status/trace 零证据文本泄漏，
  `DOD-06`）。
- M5-08：`docs/api/README.md` 接口参考索引、`docs/compatibility/` 兼容性登记、
  `THIRD_PARTY_NOTICES` 平台库分节、README 产品化更新。

### 修复

- `mirador::inverse` 对行列式非有限（±inf）或逆矩阵元素溢出的输入现在返回
  `kCoordinateTransform`（此前 det=+inf 返回 ok 且逆矩阵全零）；由模糊测试
  发现，回归测试与 fuzz 不变量双向锁定。

### 兼容性影响

- 全部为增量公共 API；M0-M4 既有 API 不变，kDisplay 开放为向后兼容扩展
  （`FusionOptions` 新增字段、`EvidenceSet` 接受空间放宽）。
- 新增可选构建面（均默认关闭）：`MIRADOR_BUILD_ADAPTERS_CAPTURE_LINUX`/
  `_WINDOWS`/`_ANDROID`、`MIRADOR_BUILD_INTEGRATIONS`（需网络拉取 pinned ncnn）、
  `MIRADOR_BUILD_FUZZ`（仅 clang）；CI 扩展至 13 job。
- 性能与跨平台结论遵循 `DEC-011` 限定：数字仅对 Linux x64 主基准环境有效；
  物理 Android/Windows 运行证据与真实模型评测记录了补跑条件
  （`RISK-2026-01`/`RISK-2026-13`）。

## [0.1.0] - 2026-09-15

M4「融合、稳定 ID 与 SoM」全部工作项落地：`mirador::fusion` 多源证据融合与稳定
ID/generation、`mirador::render` Set-of-Mark 渲染，匹配算法与关联门控冻结于
`DEC-010`（[PR #9](https://github.com/Linductor-alkaid/mirador/pull/9)，CI 10/10
全绿）。本版本完成设计 §26「首个可用版本」验收。

### 新增

- M4：统一输出模型（设计 §8）：`RegionSource` 来源位掩码、`VisualRegion`
  （stable_id/anchor/source_mask/evidence_ids）、`SemanticSnapshot`
  （generation/coordinate_space/change）与 `find_region`/`is_generation_current`
  校验辅助；`ExternalRegion` 属性袋保留 `interactive`/`role`/`enabled` 平台语义
  （`RULE-11`，视觉不内生推断）。
- M4：证据与融合（`mirador::fusion`，设计 §16）：有界 `EvidenceSet`（外部/文本/
  检测/模板四类证据、确定性证据 ID、kFrame/kOriented 输入空间）；确定性融合引擎
  `fuse_evidence`——IoU/包含门控 + 类别兼容关联（中心距离仅作 trace 观测量，
  `DEC-010`）、来源权重显式置信度、可解释 `FusionTrace`（合并观测、置信度构成）。
- M4：稳定 ID（`DEC-010`）：`StableIdTracker` 门控后贪心一对一匹配（IoU/中心位移
  门控，IoU/位移/文本相似度加权成本），保留/新建/分裂/合并事件与 generation 递增
  规则（`RULE-09`：ID 仅会话内稳定）。
- M4：会话集成：`PerceptionSession::fuse()` 发布不可变 `SemanticSnapshot`
  （首次 generation 为 1，仅 tracker 报告 bump 时递增）、`latest_snapshot()` 并发
  只读；快照携带最近一次 `ChangeReport`。
- M4：`mirador::render` 转编译目标（链接恰为 `mirador::fusion`，`DEC-013` 允许
  集合表演进）：`render_set_of_mark`（RGB8 标记图、stable_id 索引固定调色板、
  点阵数字标签与确定性遮挡规避、`mark_id -> stable_id` 映射、预算上限）、
  `grid_partition` 网格划分与坐标回映纯几何工具（设计 §17；不调用 VLM、不执行
  动作）。架构测试新增 render 断言与 `link_closure_render` 探针。
- M4：示例 `hybrid_localization_tour`（设计 §24 M4 退出场景：Accessibility 区域 +
  伪 OCR/Detector 融合 → SoM → 未变化零 Backend 复用 → 变化后 generation 拒绝
  陈旧引用）。

### 兼容性影响

- 全部为增量公共 API；M0-M3 既有 API 不变。
- `mirador::render` 由 INTERFACE 转为编译目标，链接接口恰为 `mirador::fusion`
  （`DEC-013`）；`MIRADOR_BUILD_RENDER=ON` 现要求 fusion 模块开启。
- 融合证据输入空间暂限 kFrame/kOriented（kDisplay 随 M5 平台适配定义）；SoM 背景
  暂限 rotation k0 的单平面 8 位格式（NV12/旋转背景返回显式错误）。
- 融合关联与稳定 ID 的检出质量按 `RISK-2026-10`/`RISK-2026-11` 在 M5 评测收口。

## [0.1.0-beta.3] - 2026-09-15

M3「传统视觉、检测/OCR 通用组件与视觉索引」全部工作项落地：`mirador::geometry`
线段能力、`mirador::image` 检测/OCR 通用组件与 `mirador::cache` 有界视觉索引，
集成方式与组件契约冻结于 `DEC-009`/`DEC-014`
（[PR #8](https://github.com/Linductor-alkaid/mirador/pull/8)，CI 10/10 全绿）。

### 新增

- M3：`mirador::geometry` 转编译目标（`DEC-009`）：`LineDetector` SPI（`LineSegmentSet`
  统一输出、`LineDetectRequest`、`BackendInfo` 能力查询）、线段过滤（角度/长度/置信度，
  支持 180 度绕回窗口）与传递性共线合并；一方确定性线段检测器
  `SegmentGrowingLineDetector`（梯度边缘 + 方向对齐区域生长 + 闭式 PCA 拟合 + 离群
  分段，无 libm 热路径，无状态可共享）。架构测试新增 `link_closure_geometry` 探针。
- M3：`mirador::image` 检测/OCR 通用组件（`DEC-014`）：`letterbox`（面积重采样 +
  居中填充，携带实际像素操作的精确 `Transform2D`）、`nms`（分数贪心、类别感知、
  数量上限）与 `filter_detections`、`db_postprocess_aabb`（概率图 → AABB 文本框 +
  DB unclip）与 `recover_contour_boxes`（共享确定性 8 连通域标记）、`merge_text_lines`
  与 `normalize_text`（UTF-8 安全）、`refine_small_detections`（小目标 crop-refine
  组合器，逆变换坐标恢复）。
- M3：视觉索引（`DEC-014`）：core 新增 `VisualPatchFingerprint` 契约类型与
  `make_translation` 工厂；`mirador::image` 新增 `make_visual_patch_fingerprint`
  （灰度归一化 + FNV-1a 内容哈希 + dHash）；`mirador::cache` 新增有界 `VisualIndex`
  （精确内容 / 感知哈希 / 模板 NCC 三层证据、LRU 逐出与提升、候选上限）。
- M3：新增示例 `road_segments_tour`（道路线段：检测 → 过滤 → 共线合并）与
  `icon_state_index_tour`（图标状态入库 → 扰动查询 → 复用策略），证明库不绑定单一
  平台场景。
- core：`PointF`/`RectF`/`LineSegment`/`TextRegion`/`DetectionRegion` 组件相等比较。

### 兼容性影响

- 全部为增量公共 API；M0-M2 既有 API 不变。
- `mirador::geometry` 由 INTERFACE 转为编译目标，链接接口仍恰为 `mirador::core`
  （`DEC-009`，默认构建零第三方依赖）；ELSED 本体按 `DEC-009` 作为可选适配延后。
- NV12 的 letterbox / crop-refine 暂不支持（色度填充与色度丢弃决策待定，
  `DEC-014` 参考适配范围），返回 `kUnsupportedFormat`。
- DB 后处理参考实现输出 AABB 框（`polygon`/`utf8_text` 留空）；旋转框与逐字符
  结果随首个真实 Backend 引入（`DEC-014`）。

## [0.1.0-beta.2] - 2026-09-14

M2「Backend SPI 与能力结果缓存」：Backend SPI 契约冻结（`DEC-012`）、能力结果缓存
与 `PerceptionSession` 感知闭环（`DEC-013`）全部工作项落地
（[PR #6](https://github.com/Linductor-alkaid/mirador/pull/6)，CI 10/10 全绿）。

### 新增

- M2：Backend SPI 公共契约（`DEC-012`）：`OcrBackend`/`DetectorBackend` 抽象接口、
  `BackendInfo` 能力与实现身份（含 `thread_safe` 显式声明与 `validate` 门控）、
  `TextRegion`/`DetectionRegion` 原始结果、`OcrRequest`/`DetectionRequest`（ROI 与
  空间、`min_confidence`、`max_side`、`backend_params`、`output_space`、`CachePolicy`）
  与 `ExecutionContext` 取消/deadline 通道及其判定辅助。
- M2：`mirador::cache` 能力结果缓存 `CapabilityResultCache`：`RULE-07` 全字段缓存键与
  平台稳定 128 位摘要、字节预算 LRU（淘汰顺序、替换失效、单条目超预算显式
  `kBudgetExceeded`）、请求参数摘要函数；缓存默认预算冻结于 `DEC-008`（帧 4 MiB、
  能力结果 16 MiB）。
- M2：`mirador::image` 有界 `ChangeSignature`（帧尺寸 + 指纹 + 灰度缩略图）与签名版
  `detect_change` 重载：有状态消费者只保留上一帧紧凑签名；与视图版输出位一致。
- M2：`mirador::fusion` 转编译目标（`DEC-013`，链接接口恰为 core+image+cache）与
  `PerceptionSession`：`analyze_change`（首帧 `kFirstFrame` 语义）、`run_ocr`/
  `run_detector`（确定预处理链、Backend 格式门控、按 `output_space` 坐标恢复、
  读/写/强制刷新缓存策略、取消与 deadline 显式报错）；无 runtime 示例
  `perception_session_tour` 纳入默认构建。

### 兼容性影响

- 全部为增量公共 API；M0/M1 既有 API 不变（`ChangeReason` 枚举新增 `kFirstFrame` 值，
  视图版 `detect_change` 行为与成本语义不变）。
- 公共头在 GCC、Clang、MSVC 下编译通过；Android NDK arm64-v8a 交叉编译通过。
- `MIRADOR_BUILD_FUSION=ON` 现要求 `MIRADOR_BUILD_IMAGE` 与 `MIRADOR_BUILD_CACHE`
  同时开启（configure 时校验）。
- 已知限制：NDK 侧仍仅 configure/build 验证，设备侧测试按计划在 M5 补跑；ncnn/ONNX
  Runtime 示例适配（`POST-05`）按触发条件延后。

## [0.1.0-beta.1] - 2026-09-14

M1「基础图像与变化检测」：`mirador::image` 落地 CPU 基础图像操作与分层变化检测，
为"低负载"建立首个可量化闭环（PR #3/#4/#5，CI 全绿）。

### 新增
- 可选 OpenCV 适配器 `mirador::adapters::opencv`（M1-09，`MIRADOR_BUILD_ADAPTERS_OPENCV`
  默认关闭）：`cv::Mat` → 非拥有 `ImageView` 包装（CV_8UC1/3/4 按 OpenCV 通道语义映射，
  stride 感知，ROI Mat 支持）与 NV12 约定布局（`height + ceil(height/2)` 行 CV_8UC1）
  包装，及深拷贝有界导出 `export_mat`/`export_nv12_mat`。OpenCV 由集成方通过
  `find_package` 提供，不随本项目分发。
- `mirador::image` 编译目标与有界拥有缓冲 `ImageBuffer`：显式字节预算、分配前校验、
  超限返回 `kBudgetExceeded`，NV12 多平面布局遵循冻结的 `DEC-007`。
- 确定性颜色转换 `convert_color`：文档化支持矩阵；BT.601 全范围整数定点系数，
  像素循环无浮点，跨编译器位稳定。
- core `RectI` 像素整数矩形（有效性、包含、相交、相等）与旋转感知裁剪 `crop`。
- 面积重采样 `resize_area`：任意比例整数 box 权重（含放大与非整数比），NV12 仅亮度
  路径输出灰度缩略图。
- dHash 指纹 `dhash_9x8`/`hamming_distance`/`fingerprint_similarity` 与有界组合入口
  `fingerprint()`，stride 无关。
- 分层变化检测 `detect_change`/`ChangeReport`：指纹早退 → 缩略图分块差分 → 8 连通
  变化 ROI 回映帧坐标；忽略区域、帧相似度、变化面积比例、none/partial/global 分类；
  内部缓冲固定预算。
- `mirador::cache` 编译目标与有界 LRU `FrameCache`：payload + 固定开销计入字节预算，
  超预算条目与替换显式报错且缓存状态不变。
- 变化检测基准（不变/局部变化/旋转/忽略区域四场景，p50/p95）与无 runtime 示例
  `change_detection_tour`，均纳入默认构建。

### 变更

- `resize_area` 覆盖权重改为按源行/列预计算，输出位精确不变，720p 检测路径约 3 倍
  提速（本机 release 数字）。

### 兼容性影响

- 已知限制：变化检测基准数字依赖运行机器，不做跨平台比较；OpenCV 适配器的
  Windows/macOS 编译证据待具备环境时补跑（Linux 本地 + CI 已验证）。

## [0.1.0-alpha] - 2026-09-14

M0「边界与骨架」：公共数据类型冻结、依赖边界锁定与三平台 CI
（[PR #1](https://github.com/Linductor-alkaid/mirador/pull/1)）。

### 新增

- 公共错误模型 `Status` / `Result<T>` / `Result<void>`：九类稳定错误码（无效输入、
  不支持的像素格式、坐标变换、Backend 不可用、Backend 执行失败、超时、取消、缓存损坏、
  预算超限）；异常不穿越公共 API，错误不泄漏 runtime 枚举（`DEC-004`）。
- 公共输入模型 `PixelFormat` / `Rotation` / `ImagePlane` / `ImageView` / `Frame`：
  非拥有视图、NV12 多平面表示（`DEC-007` 草案）、结构性校验 `validate()` 与
  `kMaxImageDimension` 尺寸保护。
- 坐标模型 `CoordinateSpaceId` / `Transform2D`：仿射变换与旋转/缩放/裁剪/镜像/letterbox
  工厂、空间校验的组合与求逆，点/矩形/多边形/线段统一映射（`transform_*` 命名避开
  与 `std::apply` 的 ADL 二义）。
- 架构测试（ctest 标签 `architecture`）：公共头与核心源码的线程设施/第三方库令牌扫描、
  Linux readelf 链接闭包检查、`mirador::core` 链接接口为空的 configure 断言
  （`RULE-01`~`RULE-03`）。
- CMake 模块目标 `mirador::core/image/cache/geometry/fusion/render` 与
  `MIRADOR_BUILD_<MODULE>` 按模块开关；`debug`/`release`/`warnings`/`asan`/`ubsan`/`tsan`
  及 `windows-debug` 构建预设（`DEC-005`）。
- 测试基线：GoogleTest v1.18.0 + ctest 标签机制（`unit` / `property` / `architecture`），
  坐标往返容差矩阵与固定种子属性测试。

### 兼容性影响

- 首个版本，无既有 API 影响。
- 公共头在 GCC、Clang、MSVC 下编译通过；Android NDK arm64-v8a 交叉编译通过。NDK 侧
  仅做 configure/build 验证，设备侧测试按计划在 M5 补跑。
- 测试默认开启（`MIRADOR_BUILD_TESTS=ON`），需要递归检出 googletest 子模块；最小核心
  构建可用 `-DMIRADOR_BUILD_TESTS=OFF`，不获取任何第三方依赖。

### 依赖变化

- 新增 googletest v1.18.0（BSD-3-Clause，submodule + `dependencies.lock.json` 锁定，
  仅测试目标，不进入 `mirador-core` 链接闭包）。审计记录见
  [docs/supply-chain/googletest.md](docs/supply-chain/googletest.md)。
