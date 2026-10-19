# M8：密集 OCR 的交互预算闭环

> 状态：In Progress（M8-01…04 实现落地；M8-01 真实权重实测、M8-05…07 未开始）
> 负责人：linductor（Mirador 维护者；以下各工作项同责）
> 所属计划：[Mirador 实施总计划](mirador-implementation-plan.md)
> 前置：M2/M5 已有 SPI、缓存和参考后端；基于主干 `cb45a102`
> 决策：[DEC-025](../decisions/DEC-025-budgeted-ocr-and-partial-results.md)（Accepted，M8-02 冻结）
> 建议发布点：独立 integrations 修复交付；公共增量接口随下一 minor，版本号发布前确认
> 更新日期：2026-10-19

## 目标、范围与非目标

解决 [Issue #41](https://github.com/Linductor-alkaid/mirador/issues/41) 的直接交互失败：
密集桌面帧在调用方 5 秒总预算内返回完整结果或具有足够有效文本、完整完成度说明的
部分报告。不承诺 512 行全文一定在 5 秒内识别完。

范围包含 rec 候选限额、确定性选择、调用方区域优先级、软预算、增量结果契约、会话
坐标接线、缓存隔离、接线/故障测试和真实模型基准。允许修改本 Issue 指向的自研
`integrations/ocr_ppocr` 与必要的公共抽象；不修改第三方 pinned 源码/权重、不增加
Core runtime/内部并发。M9 连续感知、实体关系和事件不是本里程碑前置。

## 工作项与交付顺序

| 工作项 | 状态 | 可评审交付物 | 依赖 | 验收 |
| --- | --- | --- | --- | --- |
| M8-01 | In Progress（harness 已交付；真实权重实测待补） | 入仓无权重基准 harness、复现指南；359 框报告复核与 512 框压力数据，分解 det/裁剪/预处理/forward/解码/收尾，固定参考环境与时间余量 | 无 | AC8-01、02 |
| M8-02 | Delivered（契约与套件已落地） | 冻结 DEC-025、新领域结果/可选能力、伪 Backend 契约套件、旧 API 兼容样例；记录配置与内存限额 | M8-01 | AC8-03、04、08 |
| M8-03 | Delivered（合成模型验证；真实模型部分随 M8-06） | PP-OCR 有界 rec 选择与软预算执行，逐框完成态、执行序和停止诊断，未识别几何可查询 | M8-02 | AC8-03…06 |
| M8-04 | Delivered | 会话新入口、坐标恢复、source 绑定与完整缓存隔离；无新能力时显式失败 | M8-02、03 | AC8-04、07、08 |
| M8-05 | Planned | 单行成本/packing/动态宽度及 batching 的独立实验报告，给出采用或 no-go 与理由；采用时单独 MR 回归 | M8-01…04 | AC8-09；no-go 不阻塞 M8-06 |
| M8-06 | Planned | 同机真实模型基准、质量对照和 5 秒配置判定，冻结参考 K/收尾预留与适用平台 | M8-01…04 | AC8-01、02、05、10 |
| M8-07 | Planned | Mirage 连续 3 次调用验收、故障/边界/sanitizer/CI 证据，API/示例/兼容性/CHANGELOG 同步 | M8-06；M8-05 的判定报告 | AC8-06…10 与全部退出条件 |

- [ ] M8-01…04 交付并通过各自验收。
- [ ] M8-05 给出可复现的实验结论；不支持 batching 也必须留下证据。
- [ ] M8-06…07 真实环境与工程门禁通过。

建议分四个可独立评审的 MR：基准与契约 → 有界执行与会话接线 → 可选优化实验 →
真实模型/下游验收收口。实验可并行开展，但不得先改默认值再补正确性证据。实现前
完成 M8-02 决策评审；本次只交付计划，不预先标记 Accepted 或 Completed。

## 验收标准与测试矩阵

以下数值为拟定门槛，未宣称已经达到；测试方法见
[OCR 预算基准协议](../benchmarks/ocr-budget-protocol.md)。修改门槛须保留原值、失败
证据和 DEC-025 修订理由，不能在同次失败后静默降低目标。

| 编号 | 可执行标准 | 证据/测试层 |
| --- | --- | --- |
| AC8-01 | 在冻结 Linux x64 参考机、PP-OCRv5-mobile、1600×1000 RGB8 上复跑基线；det=1/2/4 × rec=1/2/4，至少复现普通调度，绑核单列；记录实际框数，不要求复现未固定频点下的绝对 12.9 s | 真实模型基准，精确 commit/模型摘要/编译选项/频点与原始计时 |
| AC8-02 | 359/512 框的可读密集文本端到端场景分别测量（原报告图案另作复现）；预算配置每场景至少 20 次热调用，**每次调用返回 ≤5,000 ms**，硬超时/错误 0 次；每次至少 32 行非空且与基线一致，指定优先 16 行全部完成，其余几何逐框保留 | 从会话入口到结果可供调用方使用的计时，另列端到端/后端 p50/p95/max；不以零文本或纯 det 通过 |
| AC8-03 | 固定 K=1/64/512 与 K>N 时实际执行 min(K,N)；同分/重叠优先级有全序；相同输入重复 3 次集合/文本/顺序逐位相同；rec forward 次数等于实际识别行数（batch 时另报样本数） | 伪 Backend + tiny 模型；软预算按脚本化检查点验证，不以墙钟偶然前缀做确定性要求 |
| AC8-04 | `recognized + reused + deferred = detected`；空识别是 recognized；deferred 无伪文本/rec 分数；det 触顶/裁剪覆盖与 partial 均可见；旧 API 消费样例无需修改，旧接口仍不返回部分 vector | 新结果契约与旧 API 回归；不支持新能力显式报错 |
| AC8-05 | 稀疏 N=1/32/99：无预算或 K≥N、预算充足时全部完成；逐框文本与旧逐行基线相同、几何误差 ≤1e-4 px；同机 p95 相对回归 ≤5%（N=0 单列），预算参考配置的覆盖损失另列，不能混称无回归 | 真实权重 + 相同候选集合的差分；输出文本准确率另以 GT 核验 |
| AC8-06 | 覆盖入口/检测后/第 1 行后/中途/收尾的取消与硬超时、同时触发取消优先；软停止返回 partial；det/rec 失败、非法输出、内存不足仍整体失败；失败不发布快照、不污染完整缓存 | 故障注入；预过期 deadline 零 forward；单次 forward 不可中断延迟单列 |
| AC8-07 | 0/90/180/270 度、奇数尺寸、非连续 stride、边缘 ROI、裁剪+缩放往返；已识别和 deferred 几何都在请求空间内，往返误差 ≤1e-4 px 或说明整数裁剪误差 ≤1 px | 属性/坐标测试；prepared 空间由 Backend 输出、会话统一恢复 |
| AC8-08 | 预算入口首版不读写完整缓存，完整入口无污染；模型修订、有效参数、ROI、预处理版本变化均使旧结果失效；K/排序/优先级变更不误用整批结果；超限无静默截断 | 缓存负向矩阵、身份/内存边界；原 model_id/revision 不被错误改写 |
| AC8-09 | 实验采用门槛：相同选中行 dense rec p95 至少下降 20%，文本/几何一致，置信度最大绝对差 ≤1e-4、稀疏 p95 回归 ≤5%，预算与取消单元合格；未达标记录 no-go | 真模型；ncnn/模型 batch 能力证明、宽度分桶/pad 上限与 RSS；伪 batch 不计 |
| AC8-10 | 同一 Mirage 交互任务连续 3 次 OCR 全部在 5 秒内拿到可消费报告，完成态/行数/几何/停止原因可复核；不从 partial 自动授权动作；默认构建与可选 integrations 门禁通过 | 下游受控端到端记录 + CI + ASAN/UBSAN；性能不使用 CI runner 数字 |

det 的 `max_boxes`、rec 的 K、候选/优先级字节限额分别测试零、负值、刚好上限、上限+1、
溢出与大量文本；默认预算不被超限自动放大。实际返回字节、工作缓冲峰值和 ncnn RSS
分别报告，不把 runtime 内存混成 Core 的预算保证。

## 退出条件

- [ ] DEC-025 评审与新增接口/完成度/时间边界冻结，旧接口兼容性有证据。
- [ ] AC8-01…10 全部具备实际证据；AC8-09 可为有证据的 no-go，其他门槛不可豁免。
- [ ] Linux 六预设 debug/release/asan/ubsan/tsan/warnings 适用门禁与架构测试通过；
  integrations 专用构建和 ASAN/UBSAN 覆盖新增执行路径；涉及共享缓存时增加 TSAN/故障注入。
- [ ] Windows/Android 的新公共抽象及 integrations 构建/适用测试有 CI 证据；
  这些平台没有实机性能数据时只声明构建支持，不声明 5 秒。
- [ ] 文档、示例、兼容性、CHANGELOG、基准原始摘要与下游接入说明齐备；无权重/真实帧入仓。
- [ ] 交互验收独立完成后可交付 M8；不以 M9 尚未开始拖延修复，不提前关闭整个 Issue #41 的后续诉求。

## 风险、阻塞与补跑归属

| 风险 | 处置与负责人/条件 |
| --- | --- |
| 参考数据、权重或 Mirage 工程不在当前受控环境 | linductor 提供显式路径、模型/字典/manifest 摘要和 5 秒调用链；M8-01/06/07 保持未完成，获得后按基准协议执行 |
| 单次 det/长行 rec 超过剩余预算 | M8-01 测最大单元与收尾；M8-03 拒绝启动、缩小配置；M8-06 仍超时则门槛失败，不宣称硬实时 |
| top-K 快但漏掉有用内容 | AC8-02 同时要求非空正确行数与指定焦点覆盖，不能只报耗时 |
| 当前模型不支持真实 batching | M8-05 交付 no-go；保留有界逐行实现，不修改 pinned runtime 绕过审查 |
| 真实 OCR 本身误读 | 同基线等价与人工 GT 准确率分列，不把预算完成行数等同于文字正确率 |

## 实施与验收记录

2026-10-19：计划制定，基于最新远程主干 `cb45a10271c53d3b189bb33eef3f692d94feb562`，
阅读 Issue #41 正文及维护者补充、DEC-004/012/015/021/023/024、现有 PP-OCR/runtime/
session 源码。仅新增方案和验收协议，未执行真实权重基准、运行测试或下游验收；
所有实现项/退出条件未勾选。后续证据按工作项追加日期、commit、环境、准确命令、
结果、失败和补跑条件，禁止将 Issue 报告的中位数当作本计划实测。

2026-10-19：M8-01…04 实现（分支 `feat/m8-budget-contract`，基于计划提交
`cac1cba`；测试由 Independent-Verification-Agent 独立编写与执行，主循环仅实现与修复）：

- **M8-02 契约**：`include/mirador/ocr_batch.hpp` + `src/core/ocr_batch.cpp`
  （`BudgetedOcrBackend` SPI、请求/报告类型、校验/全序/字节核算/一致性/摘要自由
  函数）；DEC-025 转 Accepted 并附冻结记录（负优先级有效、0 仅表示无相交条目、
  NaN 排最后；`min_confidence` 只过滤 det 候选；软预算拆 `soft_budget` +
  `finalize_reserve` 两旋钮；det 触顶用 max_boxes+1 内部探测截断）。
  独立证据：`mirador.core.ocr_batch` 52/52（debug + UBSAN，零警告）。
- **M8-04 会话接线**：`PerceptionSession::run_ocr_budgeted` + `BudgetedOcrRequest`/
  `BudgetedOcrReport`；示例 `examples/budgeted_ocr_tour.cpp`（旧 API 兼容样例同
  场景演示，运行 exit 0）。独立证据：`mirador.fusion.session_budgeted_ocr` 30/30
  （k0/k90/k180/k270、33x17 奇数尺寸 + 145 stride、ROI 往返 ≤1e-4 px、入口即
  取消/超时零后端调用、取消优先、不一致报告 kBackendFailure、缓存隔离、绑定
  字段、`static_assert` 两 SPI 互不继承）；回归 core+fusion 26 套件全绿。
- **M8-03 PP-OCR 有界执行**：`integrations/ocr_ppocr/ppocr_budgeted.{hpp,cpp}` +
  共享助手重构 `ppocr_internal.{hpp,cpp}`（旧路径行为不变，`ppocr_smoke` 40/40）。
  独立证据：`mirador.integrations.ppocr_budgeted_smoke` 131/131——含输入敏感
  1x1 卷积 det 夹具产生 6 候选的确定性多行场景：K<N 选择与 3 次重复逐位相同、
  优先级执行序、det_limit_hit==true 截断（4 块 + max_boxes=3）、probe 饱和边界
  （kept ≥ cap+2 整调用显式 `kBudgetExceeded`，与旧 postprocess 失败语义同源，
  是否放宽待 M8-06 真实密度数据）、finalize 软停止（`rec_unit_upper_bound=1h`
  确定性触发）、rec 单行合成内核宽度 165 实测推导记录于测试文件头。
- **M8-01 harness**：`benchmarks/ocr_budget_bench.cpp`（确定性合成场景
  dense359/dense512/longlines/sparse、baseline/budgeted 双路径、阶段分解与逐调用
  无内容 CSV、无权重 exit 3、dense512 gate exit 4）；协议文档 §4.1 补入实际入口
  与结果格式及夹具约束。独立证据：匹配内核合成模型下 baseline/budgeted 双模式
  exit 0（SUMMARY/CSV/计数正确），缺权重 exit 3。
- **工程门禁**：debug/release/warnings/asan/ubsan/tsan 六预设 + integrations
  预设构建零警告；ctest debug 56/56、release 55/55、warnings 55/55、asan 55/55、
  ubsan 55/55、tsan 55/55、integrations 61/61。实现修复两处自查契约缺陷（K<N
  全部执行完的 stop reason；execution_order 为执行序排列而非阅读序递增）与一处
  IVA 发现的语义不一致（负优先级钳 0 → 按冻结文档改为有效值）。
- **限制与补跑条件**：AC8-01/02/05/10 需真实权重与受控场景（路径提供后按协议
  §4.1 命令补跑）；det_limit_hit 在真实密度下的行为随 M8-06 复核；会话"入口预检
  之后、后端调用之前"的 deadline 检查无法无钩子确定性触发（同函数内相邻检查，
  风险可忽略）；ncnn 前向 extract 失败路径的 stderr `pool allocator` 噪声为 M5
  既有问题，另立跟进不阻塞 M8。Windows/Android 仅构建级验证，随 CI 补充。
