# Mirador 实施总计划

> 状态：Active
> 版本：1.15
> 负责人：linductor
> 设计依据：[Mirador 低负载终端视觉基础设施库开发设计方案](../design/mirador-development-design.md)
> 协作约束：根 [AGENTS.md](../../AGENTS.md) 与[项目管理与工程规范](../project/project-standards.md)
> 更新日期：2026-09-23
>
> 1.15 修订（2026-09-23）：M7 工作项 `M7-05` 邻域验证器测试与门禁证据
> 落地，工作项勾选——Independent-Verification-Agent 验证套件 30 用例
> （`verification_roi` 冻结扩展/贴边钳制、E1 峰值与 PSR 阈值边界及平坦
> 拒绝、`DOD-04` 模板集变化使验证失效、E2 基线簿记字节记账与容差含边界
> 比较、冻结工作量公式边界、校验与预算先于取消、DOD-03 坐标矩阵、
> stride/格式不变性、逐位确定性与纯 const 决策）随验证轮处置 commit 落
> 地：`verify_track` 校验/取消优先级注释的 M7-02 出处更正为本项冻结决策
> 并注明与 `adopt_track` 入口的刻意对照（注释级修改，行为与冻结契约不
> 变）。本地门禁：debug ctest 47/47、验证套件 30 用例与既有
> `mirador.fusion.object_tracker` 72 用例直跑通过、asan/ubsan 验证套件
> 直跑零 sanitizer 报告、clang-format/clang-tidy 归零；release/tsan 等
> 其余预设与六预设完整复跑随编排脚本收口。CI 证据随 PR #25 回填（run
> 35821784682，14/14 job 全绿，PR 待合入）；`SCOPE-13` 维持未勾选（M7
> 进行中）。
>
> 1.14 修订（2026-09-23）：M7 工作项 `M7-05` 邻域验证器实现交付于工作分支
> `feat/m7-05-neighborhood-verifier`——`ObjectTracker::verify_track`（纯逐
> track 双通道证据决策：E1 验证 ROI 内多模板 NCC 峰值 + 峰旁瓣质量 PSR，
> E2 闭合结构描述量与池内基线偏差容差）、验证 ROI 查询
> `verification_roi` 与基线簿记 `record_structure_baseline`（每 track 单槽、
> 字节记账）。四项契约裁决（裸描述量消费保持 fusion 零新依赖、单槽基线、
> 负模板边界归 M7-06、ROI 扩展语义）冻结于头注释并同步设计 §6.2 落点
> 注记。本地 debug 构建零告警、ctest 46/46、clang-format/clang-tidy 自查
> 归零；测试由 Independent-Verification-Agent 独立编写与执行，工作项勾选、
> 六预设门禁与 CI 证据随验证套件落地回填。同日验证员首轮处置：验证套件
> 30 用例随验证员 commit 落地（debug ctest 47/47），实现侧更正
> `verify_track` 校验/取消优先级注释的 M7-02 出处引用为本项冻结决策
> （行为不变），并就不可达防御分支、工作量与 ROI 计量精度补头注释注记。
> `SCOPE-13` 维持未勾选（M7 进行中）。
>
> 1.13 修订（2026-09-23）：M7 工作项 `M7-04` 全局位移估计原语测试与门禁
> 证据落地，工作项勾选——Independent-Verification-Agent 验证套件 23 用例
> （已知位移恢复与冻结置信度/精度规则、胜者总序、DOD-03 坐标矩阵、预算/
> 取消/超时显式转化、格式路径逐位一致、签名重载坏状态拒绝、隐私零落盘）
> 随验证轮处置 commit 落地：签名重载缩略图尺寸一致性校验修复（契约未
> 放宽，前缩略图更大方向的 ASAN 实证越界读以回归用例锁定）与头注释精度
> 同步。本地门禁：debug ctest 46/46、asan/ubsan/tsan 全量 ctest 各 45/45
> 且 sanitizer 零报告（tsan 经 `setarch -R` 注册包装）、clang-format 全仓
> 归零、clang-tidy `--warnings-as-errors='*'` 退出码 0；release/warnings
> 预设与六预设完整复跑随编排脚本收口。CI 证据随 PR #24 回填（run
> 35808507168，14/14 job 全绿，PR 待合入）；`SCOPE-13` 维持未勾选（M7
> 进行中）。
>
> 1.12 修订（2026-09-23）：M7 工作项 `M7-04` 全局位移估计原语实现交付于
> 工作分支 `feat/m7-04-global-shift-estimation`——`mirador::image` 公共契约
> `shift_estimation.hpp`（Experimental；`estimate_global_shift` 双入口：
> `ImageView` 双帧 / M1 `ChangeSignature` 双签名；灰度缩略图
> `[-max_shift, max_shift]²` 整数平移全搜索，置信度/位移精度/默认参数口径
> 冻结于头注释，M7-09 校准；显式字节预算与 kCancelled/kTimeout 错误模型；
> 纯函数，不触碰 `ObjectTracker` 状态，消费侧归 M7-07）。本地 debug 构建
> 与 ctest 45/45、clang-format/clang-tidy 自查归零；测试由
> Independent-Verification-Agent 独立编写与执行，工作项勾选、六预设门禁与
> CI 证据随验证套件落地回填。同日验证员首轮发现签名重载缺失缩略图尺寸
> 一致性校验（前缩略图大于当前时堆越界读，ASAN 实证；反向静默误接受）
> ——补宽高相等校验修复（`detect_change` 同款检查，契约未放宽）并同步
> 头注释精度（并列零候选 confidence==1.0 告警、resize 权重表预算口径
> 交底），复验 debug ctest 46/46 与修复后 ASAN 探针通过；尺寸一致性
> 负向用例由验证员补充。`SCOPE-13` 维持未勾选（M7 进行中）。
>
> 1.11 修订（2026-09-23）：M7 工作项 `M7-03` 变化检测门控三级短路测试与
> 门禁证据落地，工作项勾选——Independent-Verification-Agent 验证套件
> 16 用例（三级分类、同帧多 track 独立短路、DOD-03 坐标矩阵、取消/超时
> 转化、确定性）随契约修正 commit 合入；本地门禁 tsan 失败定位为高熵
> ASLR 内核环境冲突（与被测代码无关），经测试注册 `setarch -R` 修复后
> 裸 `ctest --preset tsan` 44/44，debug ctest 45/45，object_tracker 直跑
> 五构建各 72/72；门控基准复跑 gate-only p50 0.198–0.244 µs 落已发布
> 0.09–0.34 µs 区间，相对 M1 基线无可测回归结论维持。CI 证据随 PR #22
> 回填（run 35759053505，14/14 job 全绿，PR 待合入）；`SCOPE-13` 维持
> 未勾选（M7 进行中）。
>
> 1.10 修订（2026-09-22）：M7 工作项 `M7-03` 变化检测门控三级短路实现与
> 基准对照交付于工作分支 `feat/m7-03-change-gated-short-circuit`——
> `ObjectTracker::evaluate_change_gate`（消费调用方 `ChangeReport` 的纯决策
> 门控，kNone 全短路/kPartial 逐 track ROI 相交判定/kGlobal 不短路）与
> `mirador_bench_change_gate` 开销对照（gate-only p50 0.09–0.34 µs，对 M1
> 基线无可测回归，数字发布于 `docs/benchmarks/`）；测试由
> Independent-Verification-Agent 独立执行，工作项勾选与 CI 证据随门禁落地
> 回填。`SCOPE-13` 维持未勾选（M7 进行中）。
>
> 1.9 修订（2026-09-22）：M7 工作项 `M7-02` 目标池有界结构交付——
> `ObjectTracker` 池有界变更原语（`record_observation`/`add_template`/
> `add_negative_template`/`advance_layout_generation`/代际分组查询与淘汰
> trace 计数）随 PR 合入；`SCOPE-13` 维持未勾选（M7 进行中）。
>
> 1.8 修订（2026-09-21）：M7 立项生效——负责人指示"依照设计与计划，继续
> 下一阶段开发"，[DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md)
> 与 [DEC-020](../decisions/DEC-020-tracker-backend-spi.md) 同轮批准转
> Accepted；M7 里程碑转 In Progress（分支 `feat/m7-cross-frame-object-tracking`），
> `M7-01` 目标池契约冻结交付（`object_tracker.hpp`，Experimental）。
>
> 1.7 修订（2026-09-21）：按负责人指示将轻量深度 tracker 升级为 M7 计划性
> 交付——新增 [DEC-020](../decisions/DEC-020-tracker-backend-spi.md)
> （`TrackerBackend` SPI 契约：有状态会话句柄、同步边界、缓存豁免界定，
> Proposed）；`POST-06` 由延后项升级为计划性交付物（NanoTrack ncnn 参考
> 后端随 M7-11~13 落于 `integrations/`，复用 `DEC-015` ncnn 基础设施）；
> 新增 `RISK-2026-18`（深度通道置信冲突/漂移污染）。DEC-019 同步修订决策
> 第 6 条（仍 Proposed）。
>
> 1.6 修订（2026-09-21）：M7「跨帧目标跟踪（低负载 SOT 与级联重检测）」
> 立项草案立档——新增 [DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md)
> （Proposed，待负责人评审）、[跟踪设计](../design/object-tracking-design.md)
> 与 [M7 里程碑](m7-cross-frame-object-tracking.md)（Proposed）；新增
> `SCOPE-13`、`POST-06`/`POST-07`、`RISK-2026-17`/`RISK-2026-16`。范围未
> 生效，随 DEC-019 评审结论确定。
>
> 1.5 修订（2026-09-20）：经用户授权发布 `v0.3.0`（tag 打在 v0.3.0 收尾 PR
> 合并提交）；批准 [DEC-018](../decisions/DEC-018-geometric-region-proposal-promotion.md)
> （Accepted），阶段 1 契约冻结生效，`geometric_proposal.hpp` 转正式并计入
> 兼容性承诺。
>
> 1.4 修订（2026-09-20）：M6 收口——`M6-06` 完成，go/no-go 判定为合成口径
> GO（`DEC-017` 四项门槛全 PASS），转正决策草案 [DEC-018](../decisions/DEC-018-geometric-region-proposal-promotion.md)
> 立档（Proposed，待负责人评审）；`SCOPE-12` 勾选，M6 状态转 Completed；
> `v0.3.0` 发布点维持暂定（收尾发布需用户授权）。
>
> 1.3 修订（2026-09-16）：M6 立项（实验轨道，issue #11；里程碑文档、`DEC-017`
> 冻结、设计 §24 增补 M6 节）；新增 `SCOPE-12`。同轮经用户授权完成 `v0.2.0`
> 发布收尾（tag 打在 PR #12 合并提交，M5 状态登记同步）。
>
> 1.2.1 修订（2026-09-16）：簿记补勾 `SCOPE-01`/`SCOPE-02`/`SCOPE-03`（M0/M1/M2
> 均已 Completed 并发布，仅复选框未随证据同步）；无范围变化。
>
> 1.2 修订（2026-09-15）：M5 立项（里程碑文档、`DEC-011`/`DEC-015` 冻结）；
> `POST-05` 立项闭环转交付中；`SCOPE-07` 补勾（M1-09 已交付）。
>
> 1.1 修订（2026-09-14）：承接设计 §13/§14 的检测/OCR 通用组件（新增 `SCOPE-11`，
> 并入 M3）；`POST-05` 参考后端由延后项升级为计划性独立交付物，立项窗口为 M4 完成后、
> M5 评测准备启动前。

## 当前状态

M0「边界与骨架」已完成并发布 `v0.1.0-alpha`（PR #1 全部工作项与退出条件通过，CI 9/9
绿）：公共类型（`Status`/`Result`、`ImageView`/`Frame`、`Transform2D`）、架构测试、
GoogleTest v1.18.0、三平台 CI 已冻结，发布说明见 [CHANGELOG](../../CHANGELOG.md)。
M1「基础图像与变化检测」已完成并发布 `v0.1.0-beta.1`（PR #3/#4/#5 合入，tag 打在
PR #5 合并提交）。M2「Backend SPI 与能力结果缓存」已完成并发布 `v0.1.0-beta.2`
（2026-09-14，PR #6 经用户授权合入，CI 两次 run 全绿；里程碑文档见
[m2-backend-spi-and-result-cache.md](m2-backend-spi-and-result-cache.md)，tag 打在
PR #6 合并提交）。M3「传统视觉、检测/OCR 通用组件与视觉索引」已完成并发布
`v0.1.0-beta.3`（2026-09-15，PR #8 经用户授权合入，CI 10/10 全绿；里程碑文档见
[m3-traditional-vision-common-components-visual-index.md](m3-traditional-vision-common-components-visual-index.md)，
tag 打在 PR #8 合并提交）。M4「融合、稳定 ID 与 SoM」已完成并发布 `v0.1.0`
（2026-09-15，PR #9 经用户授权合入，CI 10/10 全绿；里程碑文档见
[m4-fusion-stable-id-and-som.md](m4-fusion-stable-id-and-som.md)，tag 打在 PR #9
合并提交）。M5「平台适配与产品化基准」已启动（2026-09-15，立项完成：里程碑文档
见 [m5-platform-adapters-and-production-benchmarks.md](m5-platform-adapters-and-production-benchmarks.md)，
`DEC-011`/`DEC-015` 冻结，`POST-05` 立项闭环、转交付中）。M5 工作项
`M5-01`~`M5-09` 已全部收口并经 PR #12 合入 master（CI 13/13 绿；`DEC-016` 冻结，
基准/评测集/文档收口完成）。`v0.2.0` 已发布：tag 打在 PR #12 合并提交 236df8f，
GitHub Release 说明取自 CHANGELOG `0.2.0` 段。整体路线沿用设计文档第 24 节的 M0-M5。M6「几何区域 Proposal 实验
（实验轨道）」已完成并发布 `v0.3.0`（2026-09-20：`M6-01`~`M6-06` 全部交付，
PR #13/#14/#15/#17 合入（CI 14/14 绿）；`M6-04` 合成口径 `DEC-017` 四项晋升
门槛全 PASS，条件工作项 `M6-05` 触发并交付；`M6-06` go/no-go 判定为**合成
口径 GO**；[DEC-018](../decisions/DEC-018-geometric-region-proposal-promotion.md)
经用户授权批准（Accepted，两阶段转正：阶段 1 契约冻结生效、
`geometric_proposal.hpp` 计入兼容性承诺，阶段 2 融合/输出模型集成待真实
截图评估另行立项）；`v0.3.0` tag 与 GitHub Release 经用户授权发布；
[里程碑文档](m6-geometric-region-proposal-experiment.md)含完整判定记录与
口径限定）。M7「跨帧目标跟踪（低负载 SOT 与级联重检测）」已启动
（2026-09-21，立项生效：`DEC-019`/`DEC-020` 经负责人批准转 Accepted，
[里程碑文档](m7-cross-frame-object-tracking.md)转 In Progress；`M7-01`
目标池契约冻结、`M7-02` 池有界变更原语已交付，`M7-03` 变化检测门控三级
短路已交付并勾选（实现、16 用例验证套件、门控基准与门禁证据落地；CI
证据已回填：[PR #22](https://github.com/Linductor-alkaid/mirador/pull/22)
run 35759053505 14/14 job 全绿，待合入）；`M7-04` 全局位移估计原语已交付
并勾选（实现、23 用例验证套件、签名重载尺寸一致性修复与门禁证据落地于
分支 `feat/m7-04-global-shift-estimation`；CI 证据已回填：
[PR #24](https://github.com/Linductor-alkaid/mirador/pull/24)
run 35808507168 14/14 job 全绿，待合入）；`M7-05` 邻域验证器已交付并勾选
（实现、30 用例验证套件、验证员首轮出处/精度处置与门禁证据落地于
分支 `feat/m7-05-neighborhood-verifier`；CI 证据已回填：
[PR #25](https://github.com/Linductor-alkaid/mirador/pull/25)
run 35821784682 14/14 job 全绿，待合入）。

## 交付边界

### 包含

- [x] `SCOPE-01` `mirador-core`：基础类型、`Status`/`Result`、`ImageView`/`Frame`、坐标空间与
  `Transform2D`、Backend SPI 接口与能力查询（设计 §5-§9、§18-§19；M0-01~M0-08 交付，
  里程碑 Completed，发布点 `v0.1.0-alpha`）。
- [x] `SCOPE-02` `mirador-image`：颜色转换、缩放、裁剪、指纹、分块差分、变化 ROI、帧级有界
  缓存（设计 §11、§24 M1；M1 工作项全收口，里程碑 Completed，发布点 `v0.1.0-beta.1`）。
- [x] `SCOPE-03` `mirador-cache`：有界缓存与字节预算、能力结果缓存键、语义快照缓存、有界
  视觉索引（精确哈希/感知哈希/模板匹配）（设计 §12、§24 M2-M3；M2 交付缓存键与字节
  预算、M3 交付视觉索引，里程碑均 Completed，发布点 `v0.1.0-beta.2`/`v0.1.0-beta.3`）。
- [x] `SCOPE-04` `mirador-geometry`：`LineDetector` SPI、几何过滤、ELSED 或等价线段实现
  （可选依赖）（设计 §15、§24 M3；一方等价实现交付，ELSED 本体为可选适配延后，
  见 [DEC-009](../decisions/DEC-009-elsed-integration.md)）。
- [x] `SCOPE-11` 检测/OCR 通用组件：letterbox 预处理组合、NMS、类别过滤、小目标
  crop-refine；DB 后处理、轮廓框恢复、行合并、文本规范化参考组件——纯 CPU 算法，
  不执行模型、不引入 runtime，模块归属随 M3 立项确定（设计 §13、§14；1.1 修订并入）。
- [x] `SCOPE-05` `mirador-fusion`：多源证据关联、确定性融合、来源追踪、稳定 ID 与 generation
  （设计 §16、§24 M4）。
- [x] `SCOPE-06` `mirador-render`：SoM 渲染、调试叠加、网格划分与坐标回映工具（设计 §17、
  §24 M4）。
- [x] `SCOPE-07` `adapters/opencv`：`cv::Mat` ↔ `ImageView` 互操作（可选依赖，非公共 API）
  （设计 §5、§21；M1-09 已交付，产品化兼容性登记随 M5 `M5-08` 收口）。
- [x] `SCOPE-08` `benchmarks`：变化检测、缓存命中路径、Backend 调用外层耗时的基准入口与
  评测集组织（设计 §20、§23、§24 M5；数字与评测集约定见
  [docs/benchmarks/](../benchmarks/)，M5-06 收口）。
- [x] `SCOPE-09` `examples`：无 runtime 的基础示例，使用公共 API 并纳入编译验证（设计 §21；
  五个示例随全部 CI job 编译，索引见 [docs/api/README.md](../api/README.md)）。
- [x] `SCOPE-10` 多平台验证：Linux、Windows、Android NDK 的构建、测试与基准证据及 CI 门禁
  （设计 §21、§24 M0/M5；CI 13 job 全绿含 msvc/ndk/integrations/capture/fuzz，
  基准数字按 `DEC-011` 限定 Linux x64 主环境，补跑条件见
  [docs/compatibility/](../compatibility/compatibility.md)）。
- [x] `SCOPE-12` 实验性几何区域 Proposal：闭合/近闭合线段结构分析、Tight/Context
  双 ROI 与验证 harness、指标发布与 go/no-go 判定（设计 §24 M6 实验轨道、
  [DEC-017](../decisions/DEC-017-geometric-region-proposal-experiment.md)；
  实验轨道，转正另立决策——判定为合成口径 GO，
  [DEC-018](../decisions/DEC-018-geometric-region-proposal-promotion.md)
  已批准（Accepted）：阶段 1 契约冻结生效并计入兼容性承诺，阶段 2 融合/
  输出模型集成待真实数据另行立项）。
- [ ] `SCOPE-13` 跨帧目标跟踪：跟踪状态机（`kTracking/kUncertain/kLost/
  kTerminated`）与有界目标池、变化检测门控三级短路、邻域验证（模板 NCC +
  闭合结构一致性）、全局运动补偿与布局代际、丢失判定与级联重检测原语及
  身份复核、`TrackerBackend` SPI（[DEC-020](../decisions/DEC-020-tracker-backend-spi.md)，
  Proposed）与 NanoTrack ncnn 参考后端（`integrations/`，`DEC-015` 机制）
  及深度增强通道条件化融合（设计 §24 M7、[跟踪设计](../design/object-tracking-design.md)、
  [DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md)（Proposed）；
  合成验证先行，`ObjectTracker`/`TrackerBackend` 契约 Experimental 至
  go/no-go 判定后经决策冻结）。

### 明确不包含

- 模型 runtime 链接进 Core、模型权重分发与下载；runtime 适配位于独立仓库或默认构建不获取
  的 `integrations/`——具体模型后端以独立交付物形式交付（见 `POST-05`，1.1 修订后为
  计划性立项，不再是无期限延后）。
- 平台采集实现、Accessibility 服务、窗口/投影权限、输入注入与点击执行。
- VLM 调用、Agent 决策、Workflow 状态机与任务调度框架。
- 常驻线程、后台轮询、网络请求与默认持久化。

## 不可破坏约束

- `RULE-01` 依赖方向始终指向 Mirador 抽象；`mirador-core` 只依赖 C++20 标准库，不依赖
  OpenCV、ELSED、executor 或任何模型 runtime，由架构测试锁住（设计 §3、§21、§24 M0）。
- `RULE-02` 公共头不暴露 `cv::Mat`、runtime 类型或第三方私有类型；平台与第三方互操作只经
  `adapters/`（设计 §5）。
- `RULE-03` 公共 API 同步基线；核心不创建线程/定时器，不暴露 executor、future 或协程 ABI；
  取消与 deadline 仅经 `ExecutionContext` 传递（设计 §3、§9、§18）。
- `RULE-04` `ImageView` 非拥有；算法不得静默修改输入；NV12 等多平面格式不假定单连续平面
  （设计 §6）。
- `RULE-05` 所有区域输出携带 `CoordinateSpaceId`，预处理产生可组合 `Transform2D`；坐标恢复
  是核心正确性能力，不得下沉为 Backend 私有细节（设计 §7）。
- `RULE-06` 所有缓存有字节预算；输入尺寸、候选数量与细化流程有保护与预算；超限是显式淘汰
  或明确错误，不得无界增长（设计 §20）。
- `RULE-07` 能力结果缓存键至少包含规范化图像指纹、源 ID、ROI、预处理版本、Backend 名称、
  实现版本、模型 ID、模型修订与请求参数摘要（设计 §12）。
- `RULE-08` `Status`/`Result` 错误模型覆盖无效输入、不支持格式、坐标错误、Backend 不可用/
  失败、超时、取消、缓存损坏与预算超限；不泄漏 runtime 错误枚举（设计 §19）。
- `RULE-09` 稳定 ID 只在跟踪会话或缓存代际内稳定；SoM 与上层动作必须携带 `generation`
  （设计 §8、§16）。
- `RULE-10` 隐私默认：内存内处理、不落盘、不联网、不记录原始帧；持久化由调用方显式启用
  （设计 §22）。
- `RULE-11` `clickable` 等平台语义不得作为视觉内生推断；外部区域属性袋保留 `interactive`、
  `role`、`enabled` 等调用方语义（设计 §8）。
- `RULE-12` 是否调用某 Backend、任务优先级与超时由上层决定；Mirador 只提供执行组件与指标，
  不硬编码全局感知策略（设计 §10）。

## 里程碑索引

| 里程碑 | 名称 | 前置 | 建议发布点（暂定） | 状态 | 文档 |
| --- | --- | --- | --- | --- | --- |
| M0 | 边界与骨架 | — | `v0.1.0-alpha` | Completed | [m0-boundary-and-skeleton.md](m0-boundary-and-skeleton.md) |
| M1 | 基础图像与变化检测 | M0 | `v0.1.0-beta.1` | Completed | [m1-image-and-change-detection.md](m1-image-and-change-detection.md) |
| M2 | Backend SPI 与能力结果缓存 | M1 | `v0.1.0-beta.2` | Completed | [m2-backend-spi-and-result-cache.md](m2-backend-spi-and-result-cache.md) |
| M3 | 传统视觉、检测/OCR 通用组件与视觉索引 | M2 | `v0.1.0-beta.3` | Completed | [m3-traditional-vision-common-components-visual-index.md](m3-traditional-vision-common-components-visual-index.md) |
| M4 | 融合、稳定 ID 与 SoM | M3 | `v0.1.0` | Completed | [m4-fusion-stable-id-and-som.md](m4-fusion-stable-id-and-som.md) |
| M5 | 平台适配与产品化基准 | M4 | `v0.2.0` | Completed | [m5-platform-adapters-and-production-benchmarks.md](m5-platform-adapters-and-production-benchmarks.md) |
| M6 | 几何区域 Proposal 实验（实验轨道） | M5 | `v0.3.0` | Completed | [m6-geometric-region-proposal-experiment.md](m6-geometric-region-proposal-experiment.md) |
| M7 | 跨帧目标跟踪（低负载 SOT 与级联重检测） | M6 | `v0.4.0`（暂定） | In Progress | [m7-cross-frame-object-tracking.md](m7-cross-frame-object-tracking.md) |

里程碑划分、范围与退出条件以设计文档第 24 节为准；发布点为暂定映射，里程碑启动时确认
并与 tag 一一对应。M4 完成设计文档第 26 节的"首个可用版本"验收。

## 尚未冻结的决策（暂定默认值）

| 编号 | 主题 | 暂定默认值 | 负责人 | 最迟冻结 |
| --- | --- | --- | --- | --- |
| [DEC-007](../decisions/DEC-007-multiplane-image-representation.md) | NV12 等多平面格式表示 | 扩展 `ImagePlane`，不假定单连续平面（M0-03 已按草案实现） | linductor | M1 |
| `DEC-008` | 缓存默认字节预算 | 已冻结：帧 4 MiB、能力结果 16 MiB（见 [DEC-008](../decisions/DEC-008-cache-default-byte-budgets.md)） | linductor | M2 |
| [DEC-009](../decisions/DEC-009-elsed-integration.md) | ELSED 集成方式 | 已冻结：一方等价实现进 M3，ELSED 本体为可选适配延后（见 [DEC-009](../decisions/DEC-009-elsed-integration.md)） | linductor | M3 |
| [DEC-010](../decisions/DEC-010-stable-id-matching.md) | 稳定 ID 匹配算法 | 已冻结：门控后贪心一对一匹配起步，分裂/合并做事件识别与 generation 递增（见 [DEC-010](../decisions/DEC-010-stable-id-matching.md)） | linductor | M4 |
| [DEC-011](../decisions/DEC-011-benchmark-environments.md) | 基准设备清单 | 已冻结：Linux x64 主基准环境 + 方法口径；物理 Android/Windows 记录补跑条件（见 [DEC-011](../decisions/DEC-011-benchmark-environments.md)） | linductor | M5 |

已生效决策见 [docs/decisions/](../decisions/)：`DEC-001` 同步 API 与无 executor、`DEC-002`
Core 不链接模型 runtime、`DEC-003` 公共 API 不暴露 OpenCV 类型、`DEC-004` 公共边界
`Result<T>`/`Status`、`DEC-005` CMake 与构建基线、`DEC-006` GoogleTest 测试框架、
`DEC-007` 多平面图像表示。M2 新增：`DEC-012`（Backend SPI 契约）、`DEC-013`
（`PerceptionSession` 归属 fusion 与模块依赖演进）。M3 新增：`DEC-009`（ELSED 集成
方式）、`DEC-014`（检测/OCR 通用组件归属与视觉索引契约）。M5 立项新增：`DEC-011`
（基准环境与方法）、`DEC-015`（POST-05 runtime 选型：ncnn 主选、`integrations/`
存放、评测接入分层）。M5-05 新增：`DEC-016`（kDisplay 变换来源与融合开放契约：
适配层提供 kOriented→kDisplay `Transform2D`，`run_*` 输出空间保持帧族）。M6 立项
新增：`DEC-017`（几何区域 Proposal 实验轨道与契约边界：落点 `mirador::geometry`、
API 非冻结 Experimental 标记、确定性/预算底线不放宽、晋升门槛初值）。`M6-06`
收口新增：[DEC-018](../decisions/DEC-018-geometric-region-proposal-promotion.md)
（几何 Proposal 两阶段转正，2026-09-20 经用户授权批准为 Accepted：阶段 1
契约冻结已生效、`geometric_proposal.hpp` 计入兼容性承诺；阶段 2 融合/输出
模型集成以真实截图评估为前置，另行立项）。1.6 修订新增：
[DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md)（跨帧目标
跟踪能力立项：fusion 落点、双通道证据模型、显式丢失语义与级联重检测原语、
合成先行 + go/no-go 转正路径；1.8 修订经负责人批准为 Accepted）。1.7 修订新增：
[DEC-020](../decisions/DEC-020-tracker-backend-spi.md)（`TrackerBackend`
SPI 契约：有状态会话句柄制、同步/取消语义、能力结果缓存豁免界定；
1.8 修订同轮批准为 Accepted，接口字段最迟 M7-11 契约冻结时定稿）。

## 通用完成定义

- `DOD-01` 架构测试证明 Core 链接闭包仅含标准库；公共头无第三方类型。
- `DOD-02` 公共头经 GCC、Clang、MSVC 编译（Android NDK 为 Clang 交叉验证）。
- `DOD-03` 新公共行为有与风险相称的自动化测试；坐标相关变更覆盖方向、stride、奇数尺寸
  与往返容差矩阵。
- `DOD-04` 缓存相关变更验证失效路径（模型修订、参数、ROI、预处理版本），不只验证命中。
- `DOD-05` 性能声明附基准方法、环境与数字；未验证声明明确限定。
- `DOD-06` 隐私约束有负向测试（默认不落盘、不联网、日志脱敏）。
- `DOD-07` 按工程规范第 8 节完成文档同步；计划状态与验证记录更新。
- `DOD-08` 适用预设（`debug`/`asan`/`ubsan`/`tsan`）构建与测试通过。

## 建议拆分顺序

先契约后实现：每个里程碑先冻结类型与接口（含文档与伪实现测试），再填充算法实现。
先 SPI 与 Fake Backend，后真实依赖；可选依赖模块保持默认关闭并可独立裁剪。
架构边界（`RULE-01`~`RULE-03`）在 M0 用架构测试固化，后续里程碑只增不改。

## 延后项与触发条件

- `POST-01` GPU/native buffer 零拷贝路径 — 触发：M5 测量证明 CPU 连续内存路径不达标。
- `POST-02` 异步扩展接口（不破坏核心 ABI）— 触发：多个调用方证明同步封装不足。
- `POST-03` 光流/轻量特征增强变化检测 — 触发：M1 基准漏检/误检率超标。
- `POST-04` Embedder Backend 与嵌入视觉索引 — 触发：M3 图标索引命中率不足。
- `POST-07` CF tracker（KCF/MOSSE 类）进 `mirador::geometry` 可选实现 —
  触发：M7 邻域验证在快速位移场景精度不足且模板 NCC 与深度增强通道均
  不可救；无模型依赖，源码引入先许可证审查（同 `DEC-009` 流程）。
  （`POST-06` 已于 1.7 修订升级为计划性交付物，见下方"计划性交付物
  （1.7 修订）"清单。）

**计划性独立交付物（1.1 修订，自延后项升级）**：

- `POST-05` 参考能力后端交付包——OCR（如 PP-OCR mobile）与检测（YOLO 系）的示例
  Backend，验证真实 runtime 可适配性并为 M5 评测提供真实能力。按 AGENTS.md 边界存放于
  默认构建不获取的 `integrations/` 或独立仓库，不进核心发布包、不随核心版本号发布。
  立项窗口：M4 完成后、M5 评测准备启动前。**已立项（2026-09-15，[DEC-015](../decisions/DEC-015-reference-runtime-selection.md)）**：
  runtime 主选 ncnn（ONNX Runtime 为文档化备选，触发条件见决策），存放形式冻结为
  仓库内 `integrations/` + `MIRADOR_BUILD_INTEGRATIONS` 默认 OFF，评测接入分层
  （合成模型冒烟 / 使用者显式路径提供权重的真实模型评测）。交付随 M5 `M5-02`~`M5-04`
  实施。模型权重不进仓库，示例通过用户显式提供路径运行。

**计划性交付物（1.7 修订，自延后项升级）**：

- `POST-06` `TrackerBackend` SPI 与 NanoTrack ncnn 参考后端——有状态跟踪
  后端的公共契约（会话句柄制，语义见 [DEC-020](../decisions/DEC-020-tracker-backend-spi.md)，
  Proposed）与参考实现（`integrations/`，复用 `POST-05` 的 `NcnnRuntime`
  与默认零获取机制；权重不入仓、用户显式路径）。2026-09-21 按负责人指示
  由"触发后演进"升级为 M7 计划性交付（`M7-11`~`M7-13`）；定位为深度增强
  可选通道，传统双通道仍为主路径，未注入时管线零变化。许可证与模型来源
  审查随 `M7-12` 登记 `docs/supply-chain/`。

## 风险

- `RISK-2026-01` Windows MSVC 与 Android NDK CI 环境可得性受限，跨平台证据可能延迟 —
  跟进：M0-06；受限时按工程规范第 4 节记录补跑条件。
- `RISK-2026-02` ELSED 许可证与集成方式未定，影响 M3 — 跟进：`DEC-009`。
- `RISK-2026-03` 坐标/变换建模在 M0 过度设计或表达力不足 — 跟进：M0-04 与设计 §7 测试矩阵。
- `RISK-2026-04` 缓存键设计遗漏导致跨模型/参数误命中 — 跟进：`RULE-07` 与 `DOD-04` 负向测试。
- `RISK-2026-08` 检测/OCR 通用组件（尤其 DB 后处理、行合并）与具体模型的后处理约定存在
  参数化差异，参考实现的适配范围未定 — 处置：M3 立项已冻结（[DEC-014](../decisions/DEC-014-common-components-and-visual-index.md)
  组件归属 `mirador::image` 与参考适配范围表）。
- `RISK-2026-09` 一方线段检测器在真实场景（屏幕分隔线、道路边界）的检出质量未与
  ELSED 对齐 — 跟进：M3 以确定性正确性为准，M5 基准收口；触发条件见
  [DEC-009](../decisions/DEC-009-elsed-integration.md)（M3 立项新增）。
- `RISK-2026-17` 跟踪位置先验在滚动/布局突变期结构性失效（运动补偿不足或
  全局/局部变化误判）— 跟进：M7 `M7-04`/`M7-07` 与 A/B/C/D 对比矩阵 B/C
  差值、`*-scroll` 延续率；回退为收紧代际判定或位置通道降权
  （[DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md)）。
- `RISK-2026-16` `*-similar-icons` 场景跟踪 swap（impostor 负证据不足）—
  跟进：M7 负模板机制与 swap 率门槛；回退为相似外观候选一律降级
  `kUncertain`（[DEC-019](../decisions/DEC-019-cross-frame-object-tracking.md)）。
- `RISK-2026-18` 深度增强通道与传统双通道置信冲突或深度 tracker 漂移污染
  模板池 — 跟进：M7 `M7-13` 组合规则（冲突保守降级 + 高置信模板更新仅取
  双通道一致帧）；深度通道可选注入，不达标时上层可关闭，主路径不受影响
  （[DEC-020](../decisions/DEC-020-tracker-backend-spi.md)）。

## 验证记录

2026-09-13：工作空间初始化并完成骨架验证——6 个 Linux 预设（debug/release/warnings/asan/
ubsan/tsan）配置、构建、ctest 全部通过，clang-format/clang-tidy 无告警；Windows 与 Android
NDK 仅有 CI 定义未实测。证据与限制详见 [M0 里程碑验证记录](m0-boundary-and-skeleton.md)。

2026-09-13：M0-01~M0-06、M0-08 实现完成（分支 `feat/core-m0-contracts`，commit
b7d1aa3..d9f54c9）：公共类型（`Status`/`Result`、`ImageView`/`Frame`、`Transform2D`）、
架构测试、GoogleTest v1.18.0 引入与全部模块目标落地。本地 6 预设 ctest 7/7 通过，
clang-format/clang-tidy 无告警；`DEC-004`/`DEC-005`/`DEC-006` 冻结为 Accepted。跨平台
编译证据随 PR #1 的 CI 运行回填，详见 [M0 里程碑验证记录](m0-boundary-and-skeleton.md)。

2026-09-15：M5 启动（`M5-01` 立项，分支 `feat/m5-platform-adapters-and-benchmarks`）。
新增 [M5 里程碑文档](m5-platform-adapters-and-production-benchmarks.md)（工作项
`M5-01`~`M5-09`）；冻结 `DEC-011`（基准环境与方法：Linux x64 主基准环境、CI runner
不作性能证据、物理 Android/Windows 补跑条件）与 `DEC-015`（`POST-05` runtime 选型
ncnn 主选 + ONNX Runtime 备选、`integrations/` 默认零获取、评测接入分层、归因
口径），两项均 Accepted；`POST-05` 立项闭环转交付中；`SCOPE-07` 补勾（M1-09 已
交付，产品化登记随 `M5-08` 收口）。纯文档变更，无代码与构建影响。

2026-09-15：M5 首批工作项实施完成（分支 `feat/m5-platform-adapters-and-benchmarks`，
PR #10，commit 742a73c..d4ac878；全部测试由 Independent-Verification-Agent 编写
并执行）：

- `M5-02`（742a73c）：`integrations/` 骨架、pinned ncnn（20260526）、
  `NcnnRuntime` 包装、合成模型冒烟；integrations 套件 40/40、debug 回归 40/40、
  lint 双口径归零；`DEC-015` 交付路径首次落地。CI 新增 `integrations-ncnn` job
  （runner 上拉取 ncnn 构建冒烟；首轮暴露 runner CMake 不默认导出编译数据库，
  显式开关修复 c78b878）。
- `M5-06` 部分（bad4822 + c9e355b）：`mirador_bench_cache_backend` 基准入口与
  Linux x64 数字（`docs/benchmarks/linux-x64-cache-backend-2026-09.md`）；
  debug/release/asan 三口径验证通过，hit 路径不重调 Backend 不变量成立。
- `M5-03`（66fd74e + 7f37a16）：PP-OCR 参考后端（ctc 解码、det/ rec/组合管线，
  复用 M3 组件）；合成模型冒烟 40 项断言、integrations 套件 41/41、debug 回归
  40/40、asan 无报告；真实权重评测待用户提供（`RISK-2026-13`）。
- `M5-05` 部分（7b67557 + d4ac878）：Linux X11 采集适配器 + 实窗冒烟（16 项
  断言）；XWayland root 限制文档化，Xorg 分支由 CI xvfb job 覆盖；Windows/
  Android 采集适配待后续。
- `M5-04`（9d0c69c + 306dfbc）：YOLO 系参考检测后端（冻结 YOLOv5 单张量输出
  契约，letterbox/M3 nms 复用，显式候选预算）；合成模型冒烟 28 项断言、
  integrations 套件 42/42、debug 回归 40/40、asan 无报告；真实权重评测待
  用户提供（`RISK-2026-13`）。
- CI：12 job 全绿（含新增 `integrations-ncnn` 与 `capture-adapter`）。

2026-09-15：M5-05 kDisplay 契约与平台采集适配实施（分支
`feat/m5-display-contract-and-capture-adapters`，全部测试由
Independent-Verification-Agent 编写并执行）：冻结 `DEC-016`；fusion 开放
kDisplay（`FusionOptions::display_transform`，`RULE-05` 坐标恢复链扩展）；新增
`adapters/capture-windows`（GDI，Windows-only）与 `adapters/capture-android`
（MediaProjection AImageReader + Accessibility 转换，NDK 部分仅 Android 构建）。
独立验证两轮（首轮报告 optional 解引用与空集绕过两处实现缺陷，修复后复验）：
debug 41/41、capture 42/42、asan/ubsan 干净、lint 归零；Windows/Android 编译
验证随分支 PR CI 回填。详见 [M5 里程碑验证记录](m5-platform-adapters-and-production-benchmarks.md)。

2026-09-15：M5-06/07/08/09 续交付收口（分支 `feat/m5-display-contract-and-capture-adapters`，
PR #12；测试由 Independent-Verification-Agent 独立编写执行）：M5-06 体积入口与
变化检测/体积报告、评测集约定（`SCOPE-08` 勾选）；M5-07 并发矩阵 + fuzz 入口 +
隐私负向（`DOD-06`），fuzz 发现并修复 `mirador::inverse` 非有限行列式缺陷
（回归 + fuzz 不变量双向锁定，debug 43/43、tsan 42/42 零报告、transform fuzz
约 112 万 runs 零 finding）；M5-08 API 索引/兼容性登记/notices/README；M5-09
终轮证据：6 预设全绿 + 最小核心构建 + CI 13/13（终轮 head）全绿，CHANGELOG
`0.2.0` 就绪。`SCOPE-08`/`SCOPE-09`/`SCOPE-10` 具备勾选证据。`v0.2.0` tag 与
PR 合并待负责人授权。

2026-09-20：Ubuntu 20.04（focal）平台适配门禁落地（分支
`feat/ubuntu20.04-adaptation`，PR #16，验证对应 commit `093ec62`；改动仅 CI、
文档与一处测试环境假设修复，测试修复由 Independent-Verification-Agent 修改、
执行并回报证据）：

- 范围：GitHub 托管 ubuntu-20.04 runner 已退役，新增 `ubuntu20-04` CI job
  （focal 容器 + 发行版 `gcc-10`/`g++-10` 10.5.0 + CMake 3.16.3 + Ninja 1.10，
  显式 `-S`/`-B` 配置并从构建目录内跑 ctest；apt 源含 old-releases 兜底）。
  公开工具链下限：GCC/Clang ≥ 10（libstdc++ ≥ 10，`std::span` 下限）、
  CMake ≥ 3.16；focal 自带 GCC 9.4 不受支持。README/CHANGELOG 同步。
- 依据：`SCOPE-10` 多平台验证；`DEC-005` 的 CMake ≥ 3.16 基线首次拿到执行
  证据（此前 CI 只跑过 runner 自带的新版 CMake）；`DOD-06`/`RULE-10` 隐私
  负向测试语义保持。
- 验证：CI run 35483744928 全 14 job 绿；`gcc10 / ubuntu-20.04` 在 focal
  容器构建 + ctest 43/43 通过。首轮暴露裸 focal 容器无 C 编译器（GoogleTest
  声明 C project()，补装 `gcc-10` 后 Configure/Build 通过）；次轮暴露
  `Privacy.PipelineWritesNoFiles` 的"temp 快照非空"前置断言在裸容器不成立
  （裸容器 /tmp 天生为空）——本地以空 TMPDIR 忠实复现 CI 失败签名，修复为
  仅依赖 before/after 差集（迭代器健康由 error-code 上报与非空 cwd 快照共同
  兜底），修复前复现失败、修复后三种方式全绿，并通过"测试窗口内注入文件必须
  被差集捕获"的对抗性验证证明保证未削弱。
- 限制：focal 的 OpenCV 4.2、X11 采集与 `integrations/`（ncnn）面未在
  20.04 上验证（可选面默认关闭，门禁覆盖默认构建矩阵 + 全部单元/属性/架构/
  并发/隐私测试）；GCC 9.x 系明确不支持。
- 同步：`docs/compatibility/compatibility.md`（工具链矩阵 + 已知限制）、
  README「构建与测试」工具链下限、CHANGELOG Unreleased 平台支持条目、
  `.github/workflows/ci.yml`（13 → 14 job）。

2026-09-20：M6 收口（`M6-06`，分支 `feat/m6-closeout-go-no-go`，[PR #17](https://github.com/Linductor-alkaid/mirador/pull/17)，
验证对应 commit `b05f783`；纯文档变更，验证由 Independent-Verification-Agent
独立执行）：

- go/no-go 判定：**合成口径 GO**——`DEC-017` 四项晋升门槛初值依 `M6-04`
  数字全部 PASS（recall 1.000 / precision 0.875 / 重复 1 个/实体 / tight-ROI
  缩减 min 0.638），条件工作项 `M6-05` 已触发交付；完整判定记录与四项口径
  限定（`RISK-2026-14`/`RISK-2026-09`/`DEC-011`/语义边界）见
  [M6 里程碑](m6-geometric-region-proposal-experiment.md) "Go/No-Go 判定
  记录"节。
- [DEC-018](../decisions/DEC-018-geometric-region-proposal-promotion.md)
  转正决策草案立档（Proposed，待负责人评审）：阶段 1 契约冻结（去
  Experimental、计兼容性承诺）、阶段 2 融合/输出模型集成（前置真实截图
  评估或明确接受仅合成证据）；实验 API 在其批准前保持 Experimental。
- 同步：`docs/compatibility/` 补登记 Experimental API 节（不计兼容性承诺）、
  `DEC-017` 反向链接、总计划决策清单 `DEC-018` 条目、CHANGELOG Unreleased、
  issue #11 实验结论评论
  ([issuecomment-5747855883](https://github.com/Linductor-alkaid/mirador/issues/11#issuecomment-5747855883))。
- 验证：六预设矩阵退出码全 0（43/43，debug+OpenCV 44/44，tsan 经
  `setarch -R`）；最小核心构建通过且 `nm -u` 证实 geometry 闭包仅
  libc/libm/libstdc++/libgcc + 核心内部；lint 双口径归零（format 退出码 0、
  tidy 90 文件 `error:` 0 行）；文档一致性五项核对 PASS；CI run
  `35490950715` 14/14 job 全绿。
- 待用户授权：PR #17 合入 master、`v0.3.0` tag 与 Release、`DEC-018` 评审结论。

2026-09-20：`v0.3.0` 发布与 `DEC-018` 批准（分支
`feat/v0.3.0-release-bookkeeping`，用户授权"合并并清理工作分支"及"授权"
两项指令覆盖；纯文档变更，测试与验证由 Independent-Verification-Agent 执行）：

- `DEC-018` 批准（Proposed → Accepted）：阶段 1 契约冻结即日生效——
  `docs/api/README.md` 转正式条目、`docs/compatibility/` Experimental 节转
  正式登记（计入兼容性承诺）、`DEC-017` 注记豁免终止、设计 §24 M6 补收口
  状态段；阶段 2 维持真实截图评估前置、独立立项不变。
- `v0.3.0` 发布：CHANGELOG `0.3.0` 段定稿（M6 实验轨道 + focal 门禁 +
  `DEC-018` 阶段 1），tag 打在 v0.3.0 收尾 PR 合并提交，GitHub Release
  说明取自该段；里程碑文档与总计划发布点同步。
- 验证：文档一致性核对由 Independent-Verification-Agent 执行；CI 门禁随
  收尾 PR 全绿后合并。

2026-09-22：M7-02 目标池有界结构交付（实现于主循环；测试由
Independent-Verification-Agent 独立编写与执行）：

- `ObjectTracker` 新增池有界变更原语：`record_observation`（有界位置历史，
  溢出显式淘汰最旧并计入 trace，纯簿记不触碰身份/证据字段）、
  `add_template`（初始模板钉死、溢出淘汰最旧非初始模板）、
  `add_negative_template`（容量 0 显式拒绝）、`advance_layout_generation`
  与 `observations_in_generation` 代际分组查询、三个淘汰 trace 计数器。
  全部路径维持字节预算与显式淘汰/显式错误语义（`RULE-06`）；`TargetTrack`
  已冻结数据布局不变，分组以平铺存储 + 按代际过滤访问实现。
- 验证：`mirador.fusion.object_tracker` 21 个新用例（二进制内 35 → 56），
  六预设 ctest debug 45/45、其余各 44/44，asan/ubsan/tsan 直跑零报告；
  clang-format 全仓归零、clang-tidy 92 文件 `--warnings-as-errors='*'`
  退出码 0。限制：`kLost`/`kUncertain` 记录路径随 M7-06 状态机补测；
  代际推进触发判定随 M7-07 交付。证据明细见
  [M7 里程碑验证记录](m7-cross-frame-object-tracking.md)。
