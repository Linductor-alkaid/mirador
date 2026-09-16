# M6：几何区域 Proposal 实验（实验轨道）

> 状态：In Progress
> 负责人：linductor
> 所属计划：[Mirador 实施总计划](mirador-implementation-plan.md)
> 前置：M5
> 发布点：`v0.3.0`（暂定；实验轨道不改变 M0-M5 的发布语义，收尾时经用户授权）
> 更新日期：2026-09-16
> 假设来源：[issue #11](https://github.com/Linductor-alkaid/mirador/issues/11)

## 目标

以实验轨道验证 issue #11 的假设：闭合/近闭合线段结构可作为语义区域的有效低成本
先验。交付四件事：

1. **闭合结构分析**（`mirador::geometry`，实验 API）：端点邻近、交点、方向连续、
   共线与空间邻接分析，把线段组织为几何簇，从中提取闭合/近闭合结构并计算
   `closure_score`、`rectangularity`、`edge_support`。
2. **双 ROI 输出**：凸包 + 旋转最小外接矩形（OMBR）保留几何方向信息，轴对齐
   Tight ROI 供 fingerprint/VisualIndex/Cache 使用，按比例 + 最小/最大上限扩张的
   Context ROI 供 OCR/Detector refinement 使用。
3. **验证 harness 与指标**：合成场景基准入口（ground truth 由生成过程给出），
   按 issue #11 指标清单产出语义区域召回、候选精确率、重复率、ROI 缩减与时间
   稳定性；真实截图按离线数据接入约定评估（数据不入仓）。
4. **明确的重/转正判定**：按 [DEC-017](../decisions/DEC-017-geometric-region-proposal-experiment.md)
   门槛记录 go/no-go——达标则起草转正决策，不达标则记录结论并关闭或调整重跑。

M6 是假设验证而非架构承诺：不修改 M0-M5 已冻结契约，融合、缓存与输出模型全部
保持原状。

## 范围与非目标

范围：`include/mirador/geometric_proposal.hpp` 实验公共契约与 `src/geometry/`
实现（纯 CPU、标准库闭包）、`tests/geometry/` 单元与属性测试、`benchmarks/`
几何 proposal 指标入口、`docs/benchmarks/` 指标发布、`docs/api/README.md` 与
兼容性登记的 experimental 标注、总计划/设计文档/CHANGELOG 同步。
非目标：语义标签（`Button`/`Icon` 等）；进入 `SemanticSnapshot`/`EvidenceSet`
或融合管线（转正另立决策）；跨帧 `temporal_stability` 的正式契约（`M6-05`
条件工作项内仅实验探针）；真实标注数据入仓；VLM 集成；模型/网络/落盘；GPU
路径；`POST-01`~`POST-04`。

## 设计与决策依据

- [设计文档](../design/mirador-development-design.md) §7（坐标与变换）、§15
  （线段检测）、§12（缓存）、§23（评测设计）、§24 M6（实验轨道）。
- 已生效：`DEC-001`~`DEC-016`；输入类型复用 `LineSegmentSet`/`LineSegment`
  （设计 §15，M3 契约）。
- 本里程碑冻结：[DEC-017](../decisions/DEC-017-geometric-region-proposal-experiment.md)
  （实验轨道与契约边界：落点 geometry、API 非冻结、确定性/预算底线、语义边界、
  晋升门槛初值）。

## 工作项

- [x] `M6-01` 立项：里程碑文档；`DEC-017` 冻结；设计 §24 增补 M6 实验轨道节；
  总计划 1.3 修订（`SCOPE-12`、里程碑索引、状态）；关联 issue #11。
- [x] `M6-02` 闭合结构分析契约与实现：`GeometricRegionProposal` 实验类型、
  `ProposalParams`（端点邻近半径、角度容差、闭合度阈值、线段/proposal 预算）、
  几何关系分析与成簇、闭合/近闭合结构提取、三项评分；确定性输出（同输入位
  稳定）、显式预算与错误语义（`kInvalidArgument`/`kBudgetExceeded`）、不修改
  输入；全部测试由 Independent-Verification-Agent 编写执行。
- [x] `M6-03` 双 ROI 导出：凸包与 OMBR（旋转信息保留于描述量）、轴对齐
  Tight ROI、Context ROI 比例扩张 + 最小/最大上限；坐标矩阵（0/90/180/270、
  奇数尺寸、往返容差，`DOD-03`）与越界防护；与 `M6-02` 组装为完整 proposal
  输出。
- [x] `M6-04` 验证 harness 与指标发布：合成场景集（确定性生成器 + ground
  truth，覆盖矩形 UI、圆角、断裂边界、装饰性框线、纹理干扰）、基准入口计算
  recall/precision/duplicate/ROI reduction（temporal 项以多帧合成序列占位）；
  `docs/benchmarks/` 发布数字并附 `DEC-011` 口径限定；真实截图离线接入约定
  文档化（显式路径、数据不入仓，沿用 M5-06 约定）。
- [x] `M6-05`（条件，触发：`M6-04` 达 `DEC-017` 门槛）跨帧稳定性探针与缓存
  增益测量：同一合成序列上 proposal 关联的稳定性测量（实验 API，不进会话
  契约）；Geometry Descriptor + Tight ROI 作为 VisualIndex 查询补充的命中
  对照实验。不触发时记录不触发原因并保持关闭。
- [ ] `M6-06` 收尾与判定：全 Linux 预设矩阵 + 最小核心构建 + lint 双口径；
  `docs/api/README.md`/兼容性登记/CHANGELOG 同步；按 `M6-04`（及触发的
  `M6-05`）数字起草 go/no-go 记录——转正路径另立决策文档，关闭路径留档结论
  与（如适用）调整重跑条件。

## 风险与阻塞

- `RISK-2026-09`（沿用）：一方线段检测器真实场景质量直接约束本实验上限——
  处置：`M6-04` 分别报告"输入线段质量"与"闭合分析增益"两个口径，检测器缺陷
  不误判为假设失败；必要时以合成线段直接验证闭合分析层。
- 新增 `RISK-2026-14`：合成场景指标与真实场景价值的相关性未知，合成达标不
  等于假设成立——处置：真实截图离线接入约定随 `M6-04` 交付，go/no-go 结论
  必须带口径限定；转正决策必须基于真实数据或明确记录只有合成证据。
- 新增 `RISK-2026-15`：闭合结构提取的复杂度上限（成对/成簇关系分析在最坏
  情形为超线性增长）——处置：线段数与 proposal 数显式预算（`DEC-017` 第 4
  条），超限显式 `kBudgetExceeded`；基准入口报告耗时随线段数曲线。

## 测试与退出条件

- [ ] 全部 6 个 Linux 预设（debug/release/warnings/asan/ubsan/tsan）配置、构建、
  ctest 通过；最小核心构建（默认模块开关组合）不回归；触及文件
  `clang-format`/`clang-tidy` 无告警（退出码 + `error:` 行双口径）。
- [ ] 架构：`mirador::geometry` 链接闭包仍仅标准库（架构测试自动覆盖）；实验
  公共头在 `docs/api/README.md` 与 `docs/compatibility/` 登记为 experimental
  且不计兼容性承诺。
- [ ] 确定性与预算：同输入位稳定输出（含浮点路径）；线段/proposal 预算超限
  显式 `kBudgetExceeded`，非法参数显式 `kInvalidArgument`；无未界增长。
- [ ] 坐标（`DOD-03`）：Tight/Context ROI 在 0/90/180/270 旋转输入与奇数尺寸
  下的正确性与往返容差；ROI 不越出帧边界或显式裁剪。
- [ ] 评分语义：闭合结构与装饰性/断裂线段的正负边界用例；`closure_score` 等
  描述量在手算场景下与定义一致。
- [ ] 指标发布：`M6-04` 数字进入 `docs/benchmarks/`，附 `DEC-011` 口径限定与
  合成/真实数据区分；go/no-go 判定留档（转正决策草案或关闭结论）。
- [ ] 文档同步：设计 §24 M6、总计划 1.3、`DEC-017`、API 索引、兼容性登记、
  CHANGELOG（Unreleased）一致；issue #11 留下实验结论链接。

## 验证记录

2026-09-16：里程碑创建（`M6-01` 立项，分支 `feat/m6-geometric-region-proposal`）。

- 依据设计 §24 M6（实验轨道）与 issue #11 拆分工作项 `M6-01`~`M6-06`；冻结
  `DEC-017`（实验轨道与契约边界）。发布点 `v0.3.0` 暂定，收尾时经用户授权。
- 纯文档变更，无代码与构建影响。

2026-09-16：`M6-02`/`M6-03` 实施完成（分支 `feat/m6-geometric-region-proposal`，
全部测试由 Independent-Verification-Agent 独立编写与执行，共三轮验证）。

- 落地内容：`include/mirador/geometric_proposal.hpp`（实验契约：`OrientedRect`、
  `GeometricRegionProposal`、`GeometricProposalParams`、`propose_regions`）与
  `src/geometry/geometric_proposal.cpp`（端点邻近成簇 → junction 图 → 闭合度
  （环判定 + 悬挂缺口/对角线）、主轴对齐 rectangularity、edge_support；凸包
  monotone chain + rotating calipers OMBR；Tight `[floor,ceil)` 包围盒与
  Context 比例 + min/max 钳制扩张；int32 越界防护；二次方循环每 64 行轮询
  取消/deadline）。
- 评分语义（`DEC-017` 第 5 条）：junction 图含环 → closure 1.0；恰两个悬挂
  junction → `1 - gap/diagonal`（近闭合）；其余 0。junction < 3 的结构永不输出。
  语义标签不进入 proposal；`temporal_stability` 按 `DEC-017` 推迟至 `M6-05`。
- 首轮（测试编写与执行）：22 个用例（8 套件）全绿——参数校验 19 组负例、预算
  闭区间语义、非有限输入整体失败、零长段丢弃、闭合/近闭合/开链/T 形/段身接触
  正负边界、context padding 钳制、101x51 奇尺寸 0/90/180/270 手算矩阵、30° 旋转
  OMBR、int32 越界（tight 与 context 双路径）、输出顺序、位稳定 + 输入不可变、
  取消/超时、随机闭环属性测试。代理过程中的 2 个失败均为测试侧设计错误（4x4
  矩形角距恰为 radius 触发 junction 全并、旋转用例漏加平移），修正测试后通过，
  反证 junction 合并与半开 bounds 语义正确；实现侧零缺陷报告。
- 第二轮（全量回归 + lint）：6 预设矩阵全绿（debug 44/44 含 OpenCV 适配器、
  release/warnings/asan/ubsan 43/43、tsan 经 `setarch -R` 禁 ASLR 43/43，
  常规模式失败为本机内核 7.0 高熵 ASLR 环境问题）；最小核心构建通过，
  `nm -u` 证实 geometry 闭包仅 libm/libc/C++ 运行时/mirador::core；clang-format
  按授权对两实现文件做纯换行修复；clang-tidy 对实现文件报 38 个 error
  （IWYU 直接包含、braced return、use-auto、const、两函数认知复杂度超限）。
- 主循环 lint 修复：补齐直接包含、braced return、auto/const，并将
  `collect_clusters`/`build_junction_graph` 拆分为 8 个小函数；`Result` 返回
  路径用显式 `Status{...}`（花括号隐式转换不成立）。
- 第三轮（重构后复验）：6 预设矩阵再次全绿（同口径，22/22 用例在 asan/ubsan/
  tsan 下无报告）；lint 双口径三文件归零（format 退出码 0，tidy 实现/测试
  `error:` 行 0）；代理逐项核对拆分前后轮询点、合并顺序、排序与悬挂记录
  语义等价，22 用例清单与首轮一致，行为零漂移。
- API 同步：`docs/api/README.md` geometry 节登记 experimental 头（不计兼容性
  承诺）；`src/geometry/README.md`、顶层 CMake 源列表、CHANGELOG Unreleased
  同步。

2026-09-16：CI 证据回填（[PR #13](https://github.com/Linductor-alkaid/mirador/pull/13)，
run `35015056962`）：13/13 job 全绿（linux gcc/clang debug、warnings/asan/ubsan/tsan、
opencv-adapter、capture-adapters、integrations-ncnn、fuzz、windows msvc/ninja、
android ndk arm64-v8a、clang-format/clang-tidy lint）。`M6-02`/`M6-03` 交付完成；
后续工作项 `M6-04`~`M6-06` 待实施。

2026-09-16：`M6-04` 实施完成（分支 `feat/m6-proposal-verification-harness`，
基线 30069af；验证由 Independent-Verification-Agent 独立执行）。

- 落地内容：`benchmarks/geometric_proposal_bench.cpp`（仅链接
  `mirador::geometry`）——1280x800 灰度帧上五类确定性合成场景（矩形 UI 6、
  圆角 4、断裂边界 4、装饰框线 4 GT + 3 装饰、纹理干扰 494 短划线 + 3 GT，
  共 21 实体）× 双口径：Mode A 精确线段直入 `propose_regions`（闭合分析层），
  Mode B 1 px 硬边光栅化 → 一方检测器 → `merge_collinear`
  （`distance_tolerance=3.0`）→ proposal（`RISK-2026-09` 输入质量口径，含
  line-recovery 指标）；指标引擎（IoU ≥ 0.5 匹配、recall/precision/dup-max/
  tight-ROI 缩减，缩减按全部 proposal 并集）内置手算自检断言，漂移即非零
  退出；`RISK-2026-15` 耗时随线段数曲线（64~1024）；temporal 占位（±2 px
  整帧平移 5 帧，信息性，`M6-05` 前置）。
- 数字发布：[linux-x64-geometric-proposal-2026-09](../benchmarks/linux-x64-geometric-proposal-2026-09.md)
  （release，GCC 13.3.0，Ultra 5 225H）。Mode A 汇总 recall 1.000 / precision
  0.875 / dup-max 1 / tight-ROI 缩减 min 0.638——`DEC-017` 四项晋升门槛初值
  全部 PASS；precision 折损全部来自装饰场景设计负例（0.571），符合假设的
  语义边界（闭合度不区分语义框与装饰框，过滤留给上层融合）。Mode B 除圆角
  外与 Mode A 一致；圆角失败机制定位为硬边楼梯光栅化下浅对角弧弦的梯度方向
  震荡（36 px 弧仅恢复 ~8 px，闭合度趋 0 系定义正确行为），记输入质量口径，
  不作为假设失败证据，闭合层能力由 Mode A（recall 1.000）单独证明。
- 文档同步：`benchmarks/README.md` 入口表、`evaluation-scenes.md` 新增几何
  proposal 真实数据接入节（显式路径、数据不入仓、缺失即显式报错、合成/真实
  结论分开列报）、CHANGELOG Unreleased。
- Independent-Verification-Agent 验证：六预设矩阵（debug/release/warnings/
  asan/ubsan/tsan，tsan 经 `setarch -R`）全部 ctest 通过（debug 44 = 本机
  预存 OpenCV 适配器本地配置，其余 43，基准非 ctest 用例、数量不变）；新
  目标六预设齐备；harness 退出码 0、两次运行除耗时行逐字节一致（确定性）；
  自检有效性经注入验证（篡改期望值 → 退出码 1）；最小默认构建通过且
  `nm -u libmirador_geometry.a` 零第三方依赖；lint 双口径（format 退出码 0、
  tidy `error:` 0 + 退出码 0）归零；报告表格与实测输出 11/11 行逐列一致。

2026-09-16：CI 证据回填（[PR #14](https://github.com/Linductor-alkaid/mirador/pull/14)，
run `35047620484`）：13/13 job 全绿（linux gcc/clang debug、warnings/asan/ubsan/tsan、
opencv-adapter、capture-adapters、integrations-ncnn、fuzz、windows msvc/ninja、
android ndk arm64-v8a、clang-format/clang-tidy lint）。`M6-04` 交付完成；
后续工作项 `M6-05`（条件触发：门槛已达标，待收尾判定时决定是否执行）与
`M6-06` 待实施。

2026-09-16：`M6-05` 实施完成（条件触发：`M6-04` 四项门槛 PASS；分支
`feat/m6-proposal-stability-cache`，基线 5b5030f；验证由 Independent-
Verification-Agent 独立执行）。

- 落地内容：`benchmarks/geometric_proposal_reuse_bench.cpp`（链接
  `mirador::geometry` + `mirador::image` + `mirador::cache`，纯测量、无核心
  与契约改动）——实验 1 跨帧关联探针：8 实体（6 矩形 + 2 圆角）× 6 帧
  ±2 px 独立确定性抖动，相邻帧 proposal 按 tight-ROI IoU ≥ 0.60 贪心关联；
  实验 2 几何门控 VisualIndex 对照：8 个尺寸互异实体、内部图案框相对坐标、
  指纹取 tight ROI 内缩 3 px 内部裁剪，帧 0–1 建库 / 2–3 查询，distinct
  （实体独立图案）与 ambiguous（共享低对比图案、相位随帧）双变体，门控 =
  第一个双边尺寸比在 [0.80, 1.25] 的候选；内置关联与门控自检断言。
- 数字发布：[linux-x64-geometric-proposal-reuse-2026-09](../benchmarks/linux-x64-geometric-proposal-reuse-2026-09.md)
  （release，GCC 13.3.0）。实验 1：5 次转移关联率均 1.000、平均配对 IoU
  0.979–0.986。实验 2：distinct 基线/门控均 1.000（几何补充零代价）；
  ambiguous 基线 0.250 → 门控 0.625（wrong 12→6），残余 6 次 wrong 经代理
  归因探针实证全部为几何相近的相邻尺寸对（100x60↔80x50、140x90↔120x80、
  180x120↔200x140）——几何只能区分几何上可分的实体，支持 issue #11 的
  "多源匹配依据"定位。`DEC-017` 第 6 条：增益只测量、不作门槛。
- 文档同步：`benchmarks/README.md` 入口表、CHANGELOG Unreleased。
- Independent-Verification-Agent 验证：六预设矩阵 ctest 全绿（debug 44 =
  本机预存 OpenCV 配置，其余 43；基准非 ctest 用例）；新目标六预设齐备；
  release/debug/asan/ubsan/tsan 五个二进制输出逐字节一致（确定性）；最小
  默认构建不回归；lint 双口径归零；文档一致性首轮发现 1 处表格错误
  （ambiguous 门控 miss 误写为 6，实测为 wrong 6 / miss 0），主循环修正后
  复验 PASS，归因表述经代理的混淆对探针实证。

2026-09-16：CI 证据回填（[PR #15](https://github.com/Linductor-alkaid/mirador/pull/15)，
run `35053412807`）：13/13 job 全绿（linux gcc/clang debug、warnings/asan/ubsan/tsan、
opencv-adapter、capture-adapters、integrations-ncnn、fuzz、windows msvc/ninja、
android ndk arm64-v8a、clang-format/clang-tidy lint）。`M6-05` 交付完成；
唯一剩余工作项 `M6-06`（收尾与 go/no-go 判定）待实施。
