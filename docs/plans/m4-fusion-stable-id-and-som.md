# M4：融合、稳定 ID 与 SoM

> 状态：In Progress
> 负责人：linductor
> 所属计划：[Mirador 实施总计划](mirador-implementation-plan.md)
> 前置：M3
> 发布点：`v0.1.0`（暂定，对应设计 §26「首个可用版本」验收；tag 与发布须经用户授权）
> 更新日期：2026-09-15

## 目标

落地设计 §24 M4 的融合能力线，全部保持纯 CPU、零模型 runtime、零第三方默认依赖：

1. `SCOPE-05` `mirador::fusion`：多源证据关联（外部结构化区域、OCR 文本框、检测框、
   模板匹配候选）、确定性融合、来源追踪（`source_mask` + `evidence_ids`）、显式置信度
   规则、跨快照稳定 ID 与 generation 校验（设计 §16、§8）。
2. `SCOPE-06` `mirador::render`：`SemanticSnapshot` → Set-of-Mark 图像与
   `mark_id -> stable_id` 映射，标签放置与遮挡规避、确定性配色；网格划分与坐标回映
   工具（设计 §17）。
3. 以 Android 混合定位场景示例验证 Accessibility 外部区域、OCR 与 Detector 可以在不
   侵入上层 Agent 的情况下组合（设计 §24 M4 退出要求），SoM 输出可交付离散区域而
   Mirador 不调用 VLM、不执行动作。

M4 完成后总计划 `SCOPE-05`/`SCOPE-06` 勾选，设计 §26「首个可用版本」中依赖代码能力
的条目（确定性融合、稳定 ID/generation 阻止陈旧使用、SoM 输出）具备验收证据。

## 范围与非目标

范围：`mirador::fusion` 新增统一输出模型契约（`RegionSource`/`VisualRegion`/
`SemanticSnapshot`）、证据模型（`ExternalRegion`/`EvidenceSet`）、确定性融合引擎与
诊断 trace、`StableIdTracker`（门控后贪心，`DEC-010`）；`PerceptionSession` 叠加
`fuse()`/`latest_snapshot()`（`DEC-013` 同一模块演进）；`mirador::render` 转编译目标
并承载 SoM 渲染与网格工具（链接接口恰为 `mirador::fusion`，`DEC-013` 允许集合表按
冻结决策演进）；一个新示例；架构测试、文档与决策同步。
非目标：VLM 调用、动作执行、prompt 构造（设计 §17 边界）；光流/嵌入相似度匹配
（`POST-03`/`POST-04`）；置信度校准（待标注数据，§16）；平台采集与 Accessibility
真实接入（M5）；基准数字（M5，`SCOPE-08`）；kDisplay 坐标空间的证据转换（依赖平台
适配，M5 与采集层一起定义变换来源）。

## 设计与决策依据

- [设计文档](../design/mirador-development-design.md) §7（坐标恢复）、§8（统一输出
  模型：`VisualRegion`/`SemanticSnapshot`/来源掩码、`interactive`/`role`/`enabled`
  属性袋）、§16（关联信号、确定性可配置可解释的融合规则、来源权重置信度、门控后
  贪心稳定 ID、generation 校验）、§17（SoM 渲染职责边界、网格细化工具）、§18
  （会话状态模型、不可变快照并发读）、§20（预算保护）、§24 M4、§26（验收）。
- 已生效：`DEC-001`~`DEC-009`、`DEC-012`、`DEC-013`、`DEC-014`。
- 本里程碑冻结：`DEC-010`（稳定 ID 匹配算法：门控后贪心匹配起步；关联门控以 IoU 与
  包含关系为主，中心距离仅作 trace 观测量）。

## 工作项

- [x] `M4-01` 立项：里程碑文档、`DEC-010` 冻结、总计划状态更新。
- [x] `M4-02` 统一输出模型契约：`RegionSource` 位掩码、`VisualRegion`（stable_id/
  anchor/source_mask/evidence_ids）、`SemanticSnapshot`（generation/coordinate_space/
  change）、`ExternalRegion` 属性袋（interactive/role/enabled，`RULE-11`）、快照查询
  与 generation 校验辅助函数。
- [x] `M4-03` 证据与融合引擎：`EvidenceSet`（外部/文本/检测/模板四类证据、确定性
  证据 ID、数量上限）、`FusionOptions`（关联阈值、来源权重、区域数预算）、确定性
  关联（IoU/包含 + 类别/文本兼容门控，同坐标空间转换）、聚类输出 `VisualRegion`、
  `FusionTrace`（哪些证据合并、使用哪条规则、置信度如何产生，设计 §16 可解释性）。
- [x] `M4-04` 稳定 ID 跟踪：`StableIdTracker`——门控（IoU/中心位移）后贪心一对一
  匹配（成本 = IoU/中心位移/文本相似度加权，确定性平局），保留/新 ID/分裂/合并事件
  与 generation 递增规则（`DEC-010`、`RULE-09`）。
- [x] `M4-05` 会话集成：`PerceptionSession::fuse()`（证据坐标转换 → 融合 → 稳定 ID
  → generation → 发布快照）、`latest_snapshot()`（不可变 `shared_ptr` 并发读），
  快照携带最近一次 `ChangeReport`；取消/超时与预算错误显式传播。
- [x] `M4-06` `mirador::render` 转编译目标（链接恰为 fusion）：`render_set_of_mark`
  （确定性数字标记、标签放置与遮挡规避、固定配色、`mark_id -> stable_id` 映射、
  预算上限）、网格划分与坐标回映工具；架构测试允许集合表演进 + render 链接闭包
  探针（`DEC-013` 影响条款落地）。
- [x] `M4-07` 示例：`hybrid_localization_tour`（合成屏幕 + Accessibility 外部区域 +
  伪 OCR/Detector Backend → 融合 → SoM → 界面变化后 generation 拒绝陈旧区域），
  纳入默认构建与编译验证。
- [x] `M4-08` 收尾：全 Linux 预设矩阵与 lint 通过、跨平台 CI 证据回填、验证记录与
  计划状态更新、CHANGELOG/README 同步。

## 风险与阻塞

- `RISK-2026-01`：MSVC/NDK 跨平台编译证据依赖 CI，受限时按规范记录补跑条件。
- 新增 `RISK-2026-10`：融合关联规则与真实场景（重叠按钮+文本、相邻图标与文本）的
  匹配边界未与标注数据对齐 — 处置：M4 以确定性正确性与可解释 trace 为准，阈值全部
  显式可配置；检出质量按 `SCOPE-08` 在 M5 评测收口。
- 新增 `RISK-2026-11`：贪心一对一匹配对分裂/合并场景只做事件识别，不做多对多最优
  匹配，极端布局变化下的 ID 保持率有限 — 处置：`DEC-010` 明确起步范围与触发条件
  （M5 基准不达标再评估二分图匹配），不静默扩大范围。
- `RISK-2026-09`（沿用）：一方线段检测器质量在 M5 基准收口，与本里程碑无直接耦合。

## 测试与退出条件

- [ ] 全部 6 个 Linux 预设（debug/release/warnings/asan/ubsan/tsan）配置、构建、ctest
  通过；触及文件 `clang-format`/`clang-tidy` 无告警。
- [ ] 融合关联：IoU/包含门控的正负边界、类别不兼容不合并、相邻但语义不同不因距离
  合并；输出确定性（同输入位稳定）；`source_mask`/`evidence_ids` 与 trace 一致。
- [ ] 坐标：证据从 kFrame/kOriented 转换到目标空间经方向（0/90/180/270）与奇数尺寸
  矩阵验证（`DOD-03`）；不支持的空间显式拒绝。
- [ ] 稳定 ID：小幅位移保留 ID、大幅位移换新 ID、分裂/合并事件与 generation 递增、
  文本相似度参与匹配、确定性平局；`RULE-09`（会话内稳定）有边界用例。
- [ ] 会话：`fuse()` 全链路（伪 Backend）端到端、未变化画面复用路径不被破坏、快照
  不可变（发布后读取不受后续 fuse 影响）、取消/超时零 Backend 调用、预算超限显式
  `kBudgetExceeded`。
- [ ] SoM：标记映射完整（每个区域一个 mark）、绘制确定性（同输入位稳定）、标签
  遮挡规避确定性、网格划分与坐标回映往返一致；`max_marks` 预算显式错误。
- [ ] 架构测试演进后：render 链接恰为 fusion、fusion 闭包仍仅标准库；公共头与
  `src/` 无第三方与线程令牌；CI 全部 job 运行。
- [ ] `DEC-010` 冻结为 Accepted；设计文档 §8/§16/§17 按冻结契约同步（如有偏差）。

## 验证记录

2026-09-15：里程碑创建。依据设计文档 §7/§8/§16/§17/§18/§24 M4/§26 与总计划
`SCOPE-05`/`SCOPE-06` 拆分工作项 `M4-01`~`M4-08`；`DEC-010` 冻结稳定 ID 起步算法
与关联门控范围。发布点 `v0.1.0` 暂定，待 M4-08 后经用户授权打 tag/发布。

2026-09-15：`M4-02`~`M4-07` 实施完成（分支 `feat/m4-fusion-stable-id-som`，commit
c255f1c..15217d8 及后续 lint/修复提交，每工作项一组 commit）。

- 环境：Ubuntu 24.04 x64（GCC 13.3.0、CMake 3.28.3 + Ninja、clang-format/
  clang-tidy 18.1.3）。
- 落地内容：
  - `M4-02`：`semantic_snapshot.hpp/cpp`（`RegionSource` 位掩码运算、
    `VisualRegion`/`SemanticSnapshot`、`find_region`/`is_generation_current`）、
    `evidence.hpp/cpp`（`ExternalRegion` 属性袋、`EvidenceSet` 确定性证据 ID 与
    kMaxItems 预算）。
  - `M4-03`：`fusion.hpp/cpp` + 私有 `rect_math.h`（`fuse_evidence`：空间转换、
    IoU/包含门控 + 类别兼容、union-find 聚类、来源权重置信度、`FusionTrace`
    association 观测；O(n²) 扫描每 64 行轮询取消）。
  - `M4-04`：`stable_id_tracker.hpp/cpp`（门控贪心一对一、文本相似度
    Levenshtein 信号、split/merge 事件、保留比例 generation 规则、失败不改状态）。
  - `M4-05`：`PerceptionSession::fuse`/`latest_snapshot`/`last_stable_id`，快照
    携带最近 `ChangeReport`，generation 首发为 1、仅 bump 递增。
  - `M4-06`：`set_of_mark.hpp/cpp`（RGB8 `MarkedImage`、调色板描框、点阵数字标签
    芯片与遮挡规避）、`grid_partition.hpp/cpp`；render 转编译目标（链接恰为
    fusion）+ 架构断言与 `link_closure_render` 探针。
  - `M4-07`：示例 `hybrid_localization_tour`（四幕：融合+SoM → 未变化复用 →
    全局像素变化保留 ID → 整屏切换换新 ID + generation 拒绝陈旧引用）。
- 测试（委派 Independent-Verification-Agent 编写并执行）：新增 7 个测试目标共
  82 个用例——`mirador.fusion.evidence_set`(8)/`fusion_engine`(21，含 property)/
  `stable_id_tracker`(19)/`snapshot`(4)/`session_fuse`(8)/
  `mirador.render.set_of_mark`(16)/`grid_partition`(9)。覆盖门控上下边界（含
  nextafter）、类别兼容、输出与 trace 确定性、置信度公式、kFrame↔kOriented 经
  0/90/180/270 与 5x3 奇数尺寸手算矩阵、稳定 ID 保留/新建/门控边界/文本信号/
  split/merge/保留比例 bump/平局/失败原子性、会话端到端与不可变快照、SoM 像素/
  调色板/标签避让/NV12 与 rotated 拒绝/预算、网格 ceil 与往返容差。
- 验证代理首轮发现 1 个实现缺陷（`assemble_region` 未填充
  `VisualRegion::evidence_ids`，违反 `semantic_snapshot.hpp` 字段契约），主循环
  修复（commit 6675d6a）后复验。
- 本地最终口径：debug 40/40、asan 39/39、ubsan 39/39、warnings（-Werror）39/39、
  tsan 39/39（经 `setarch "$(uname -m)" -R` 禁用高熵 ASLR，连续 3 轮稳定；首轮
  失败系旧二进制未重建）全部通过，无 sanitizer 报告；触及文件 clang-format 无
  告警。
- lint 往返（如实记录）：首轮 CI 的 clang-tidy job 失败（IWYU 直接包含、
  认知复杂度、聚合体成员函数、C 数组等，波及实现与测试两侧）。根因之一是本地
  验证缺陷：`--warnings-as-errors` 下诊断行是 `error:` 而本地检查 grep 了
  `warning:` 且退出码被管道掩盖，误判通过（M3 同类教训重演）。处置：实现侧
  （IWYU 直接包含、`EvidenceItem`/`MarkedImage` 改纯聚合 + 自由函数、
  `advance`/`render_set_of_mark` 拆分至复杂度阈值下、大写字面量后缀、
  `std::array` 表、`take_value` 移动）由主循环逐文件按退出码 + `error:` 行数
  双口径复验归零；测试侧错误清单交独立验证代理修复并按同一正确口径复验
  （7 文件 exit=0、error 行 0）。
- 限制：跨平台编译证据（MSVC/NDK）待 CI 运行回填（`M4-08`）；融合关联与稳定 ID
  在真实场景的质量按 `RISK-2026-10`/`RISK-2026-11` 在 M5 评测收口。

2026-09-15：`M4-08` 完成，跨平台 CI 证据回填。

- CI 往返共三轮（[PR #9](https://github.com/Linductor-alkaid/mirador/pull/9)）：
  run `34882271535` lint 失败（IWYU/复杂度/聚合体成员函数/C 数组等 38 处，暴露本地
  tidy 验证缺陷）；run `34889563564` 修复后仅剩探针文件 2 处包含错误；run
  `34892045507`（commit 984131c）**10/10 job 全绿**：linux gcc/clang debug、
  gcc warnings/asan/ubsan/tsan、gcc opencv-adapter、windows msvc/ninja、
  android ndk arm64-v8a、clang-format/clang-tidy lint。
- 本轮全口径：6 个 Linux 预设 + OpenCV 适配 18/18 通过；全部触及文件
  clang-format 与 clang-tidy（退出码 + error 行双口径）归零。
- 待用户授权事项：合并 PR #9、打 `v0.1.0` tag 与 GitHub Release（设计 §26 首个
  可用版本发布点）；完成后本里程碑转 Complete 并更新 CHANGELOG 版本段。
