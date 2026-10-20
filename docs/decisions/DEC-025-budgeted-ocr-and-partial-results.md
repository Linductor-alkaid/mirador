# DEC-025：密集 OCR 的候选预算、部分结果与优化顺序

> 状态：Accepted（M8-02 冻结，2026-10-19；性能配置仍在 M8-06 实测后冻结）
> 日期：2026-10-19
> 负责人：linductor（Mirador 维护者）
> 冻结里程碑：M8-02；性能配置在 M8-06 实测后冻结
> 替代/被替代：无；不修改 DEC-004/012/021 的既有接口语义
> 工作项：[M8](../plans/m8-budgeted-ocr.md)

## 背景与问题

[Issue #41](https://github.com/Linductor-alkaid/mirador/issues/41) 报告 Mirage 的整屏
OCR 三次耗尽 5,000 ms 预算。报告基线为 `cb45a102`、PP-OCRv5-mobile、1600×1000
RGB8、359 个检测框：单线程 det+rec 中位约 12.9 s，四线程普通调度约 20–23 s。
这些是下游报告，尚未由本计划复现；512 框耗时是外推，不能当作实测。

在同一版本源码中核对到：

- `PpOcrDetOptions` 与 `PpOcrRecOptions` 已分别提供 `num_threads`，默认均为 1。
- `PpOcrBackend::recognize` 按阅读序逐框裁剪、逐次调用 rec，没有 rec 数量预算。
- `NcnnRuntime::run_multi` 在 forward 前后分别检查取消与 deadline，超时返回
  `kTimeout`；组合循环另有取消检查。不是完全没有 deadline，而是失败时不携带已完成行。
- 单次 forward 不可中断；`run_multi` 的多 blob 接口不等于多行 batching。
- `Result<vector<TextRegion>>`、会话坐标恢复和能力缓存只表达完整成功或错误。
  空文本同时可能表示 det-only 或识别为空，不能用它编码“尚未识别”。

## 决策

### 1. 先控制工作量，再验证单行优化与 batching

交付顺序为：可复现基线 → 增量结果契约 → top-K/调用方优先级与提前收尾 →
缓存/坐标接线 → 实测 → 可选单行与 batching 优化。M8 的交互闭环不依赖 M9
连续感知，也不要求 batching 成功。

rec 保持单线程参考配置；det=4、rec=1 仅作为调用方显式性能配置参加对照，不修改
库默认值，也不由库设置 CPU affinity、OpenMP 环境或创建跨行 worker。
`NcnnRuntime` 当前为平面 CHW 关闭 packing；可在 rec 专用实验中核对张量复制、
packing 和动态宽度成本，但不得全局打开 packing 影响 YOLO/NanoTrack，或改变
预处理数值后仍声称与旧模型等价。

### 2. 新增可选能力，旧接口继续全有或全无

暂定新增独立、显式注入的 `BudgetedOcrBackend` 能力及
`PerceptionSession::run_ocr_budgeted` 入口；名称/声明由 M8-02 定稿。旧
`OcrBackend::recognize` 和 `run_ocr` 不更换返回类型、不新增必实现的虚函数，旧
Backend 无需适配；不支持新能力时显式返回 `kBackendUnavailable`，不暗中退回全量识别。
新接口只传图像、领域结果与执行上下文，不暴露 runtime 张量或调度类型。

`Result<OcrBatchResult>` 的值表示一次有效、有完整诊断的执行报告，报告至少包括：

| 字段组 | 必须表达的事实 |
| --- | --- |
| 请求绑定 | source、帧序列、实际 ROI、输出空间、模型/实现身份、请求摘要 |
| 检测覆盖 | 实际检测框数、检测上限、是否触顶/发生截断、覆盖的请求区域 |
| 每框记录 | 本次候选 ID、几何、det 分数、可选 rec 分数、文本及来源 |
| 每框完成态 | `recognized`、`reused`、`deferred`；空字符串不代替完成态 |
| 总体完成态 | `complete` 或 `partial`，明确完成范围；不代表整屏所有文字均已召回 |
| 停止原因 | 正常完成、rec 数量上限、软预算；计数与实际 rec forward 次数 |

计数必须满足 `detected = recognized + reused + deferred`，且每框只出现一次。
`recognized` 包括成功识别为空文本的行；`deferred` 保留 det 几何而不伪造文本或
rec 置信度。M8 首版可令 reused 恒为 0，M9 接入复用时沿用相同来源语义。
检测达到上限时至少标明覆盖不确定；不能把“rec 全部完成”解释为整屏完整。

新请求/报告的参考限额暂定为优先级条目最多 64 个且总计 ≤64 KiB、报告拥有内存
≤16 MiB，均需显式校验并可由调用方调小；与 det/rec 各自工作缓冲预算分开记账。
条数限额不替代字节核算，超限返回 `kBudgetExceeded`，不能静默裁掉候选/文字。

部分结果只供查询、展示或安排后续识别，默认不自动进入完整 `SemanticSnapshot`
或动作/SoM 授权路径。未来若允许基于部分观测行动，必须另修 DEC-021；本次不放宽
“失败后的部分结果不得作为动作依据”协议。

### 3. 区分软预算、硬 deadline 和显式取消

5,000 ms 是下游交互总预算，不是库可任意用满的 rec 时间。调用方传入绝对硬
deadline；新入口支持在此前停止启动新工作的软预算，并为最后一个不可中断单元、
坐标恢复、序列化及调用方开销预留时间。

- 暂定参考配置：`max_rec_lines=64`、收尾预留 500 ms；均为**显式启用配置**，
  不是已验证默认值。M8-01 测量最长单元/收尾后校准，M8-06 冻结。
- 开始 det/rec 前检查剩余时间；不足一个经测量的单元上界与收尾余量时不启动。
  det 也不可中断，入口已来不及完成 det 时返回 `kTimeout`，不承诺凭空生成几何。
- 正常达到 K 或软预算，可在硬 deadline 前返回 `partial` 值，停止原因必须显式。
- 显式取消始终返回 `kCancelled`；硬 deadline 到达始终返回 `kTimeout`；两者
  同时可见时取消优先。不把硬超时、分配超限或 runtime 失败包装为成功的 partial。
- 无效输入、非法模型输出、`kBackendFailure`、内存 `kBudgetExceeded` 保持整体错误；
  不采纳已完成前缀、不修改完整缓存/已发布状态。校验先于新工作的执行。

不向 `ErrorCode` 添加“成功的 kPartial”，不改变 `Result<T>::ok()` 的含义。软预算
是主动减少计划工作量，硬 deadline 仍按 DEC-012 返回明确错误。单次推理协作取消
无法保证任意设备上的硬实时；若实测仍越过 5 秒门槛，该配置验收失败，必须缩小
单元/预留或显式缩小支持范围，不能用调用方杀超时掩盖库未返回。

### 4. 识别选择与输出排序分开

`max_boxes` 继续限制 det 后处理，新增 `max_rec_lines` 单独限制 rec，不通过降低
det 上限丢掉几何。K 必须为正且受配置/字节预算约束，实际最多识别 min(K,N) 行；
0、负数、溢出显式拒绝；
需要 det-only 时使用已有 det Backend。

新请求支持阅读序、det 分数优先及调用方 ROI/候选优先级。执行排序暂定为：调用方
优先级降序 → 所选策略 → 几何与本次候选 ID 作为全序 tie-break。相同图像与固定
K 配置应选中同一集合；最终几何列表仍按确定阅读序输出，并带执行序号。调用方
优先级不修改 det/rec 置信度；M8 不要求先有稳定实体 ID，M9 负责把实体焦点映射成
ROI/候选优先级。所有优先级条目有数量和字节上限。

### 5. 完整结果缓存与时间受限结果隔离

保留现有完整能力结果缓存格式和键字段。M8 新预算入口首版**不读写完整结果缓存**，
防止 deadline 改变的前缀、候选排序和缺识别行污染全量命中；不把绝对时间放进缓存键。
旧入口的同输入确定性与命中语义保持不变。

M9 若增加逐行缓存，键至少包括精确裁剪内容摘要、坐标/ROI、预处理版本、模型 ID/
修订、实现版本、语言与有效参数；改变 K/选择策略/注意配置不能错误复用整批列表。
逐行内容等价命中与整批完成度是两个层次。截止时间下完成行数依赖运行速度，不应
要求与固定 K 一样逐位确定；可重复性检查须分开。

原模型 `model_id/model_revision` 不为调优伪造新权重身份；有效尺寸、线程、预处理
与选择配置必须进入实现身份或请求摘要。改变数值路径时升级 implementation_version。
缓存语义“不变”指旧 API 仍然正确，不是新增参数可省略失效登记。

### 6. batching 设为有退出条件的实验

只有 pinned ncnn、实际 PP-OCR 模型确实支持独立样本维度与逐行输出拆分时，才测试
按宽度分桶与有限 pad 的批处理。不把 CHW 的 C 维或多行纵向拼接冒充 batch，也不
因为 `run_multi` 接受多个 blob 就宣布可用。需另测 padding 对 CTC 序列、文本、
置信度和内存的影响；模型需重新导出或 runtime 升级时走独立审查。

启用门槛：相同选中行、相同权重下，密集 rec p95 至少降低 20%，文本与几何通过
对照，无稀疏回归且单批剩余时间/内存可接纳。达不到或模型不支持，记录 no-go、
保留逐行有界路径，不阻塞 M8-06；不得默认把一批扩大到无法及时取消的粒度。

## 备选方案

| 方案 | 取舍 |
| --- | --- |
| 只增加线程/绑核 | 已报告负优化，且不能减少 359–512 次前向；仅保留配置对照 |
| 直接降低 det max_boxes | 最便宜，但丢掉未识别几何，无法解释覆盖与任务优先级 |
| 旧 vector 中留空文本或错误中附带 vector | 旧消费者无法区分未识别/空文本，破坏 Result 与缓存契约 |
| 一开始只做 batching | 模型/张量能力未证实，不能保证修复交互失败 |
| 首先做完整连续感知 | 收益大但涉及融合、陈旧性与事件契约，独立 M9 推进 |

## 影响与风险

新增公共领域类型须同步 API 索引、示例、兼容性与 CHANGELOG，标明消费者重编译要求。
runtime 代码只在 `integrations/`；本 Issue 明确授权修复该适配，pinned 依赖与权重不改。
过小 K 可能只快而无用，故验收同时约束有效行数、焦点覆盖、质量与总耗时。设备降频、
长行和收尾成本可能使预算估计失效，必须在基准报告中暴露最坏样本和超时次数。

## 验证方式

[M8 验收矩阵](../plans/m8-budgeted-ocr.md)与
[OCR 预算基准协议](../benchmarks/ocr-budget-protocol.md)。必须同时有伪 Backend
契约测试、合成 tiny 模型接线测试和显式真实权重实测；仅合成测试不证明 5 秒 SLO。

## 关联文档和工作项

[设计 §13/18/19/20](../design/mirador-development-design.md)、DEC-004、DEC-008、
DEC-012、DEC-015、DEC-021、DEC-024；M8-01…07；后续
[DEC-026](DEC-026-continuous-perception-and-attention.md)。本文件为待评审方案，未宣称
API 已存在、性能达标或既有决策已被替代。

## M8-02 冻结记录（2026-10-19）

随 M8-01…04 实现（`feat/m8-budget-contract` 分支）冻结以下名称与语义；实现以头文件
注释为准，本节记录与上文"暂定"措辞的差异：

- 名称定稿：`BudgetedOcrBackend::recognize_budgeted`（独立抽象类，不继承
  `OcrBackend`）、`OcrBatchRequest`/`OcrBatchResult`/`OcrLineRecord`/
  `OcrPriorityEntry`、`OcrCompletionState`/`OcrBatchStopReason`/
  `OcrBatchCompletion`/`OcrSelectionStrategy`；会话侧
  `PerceptionSession::run_ocr_budgeted(frame, BudgetedOcrBackend*, BudgetedOcrRequest,
  ExecutionContext)` 返回 `BudgetedOcrReport`（执行报告 + source/帧序/executed_roi/
  output_space/后端身份/请求摘要绑定，行几何已恢复到 output_space）。
- `min_confidence` 只在 det 阶段过滤候选；rec 分数如实报告、不按它扣文本——旧
  rec "低于阈值返回空"的语义不带入预算报告（否则空文本会与"识别为空"混淆）。
- 软预算拆成两个显式旋钮：入口相对 `soft_budget` 与截止相对 `finalize_reserve`
  （后者必须配硬 deadline，否则会话入口拒绝）；任一触发都以
  `kSoftBudget` 停止新行并返回 partial。
- det 触顶可见化：预算后端用 max_boxes+1 内部探测截断（前缀与原限幅逐位一致），
  `det_limit_hit=true`；旧路径在超限时仍整体报 `kBudgetExceeded`，行为不变。
- 限额常量 `kOcrBatchMaxPriorityEntries=64`、`kOcrBatchMaxPriorityBytes=64 KiB`、
  `kOcrBatchMaxReportBytes=16 MiB` 只能调小；`max_rec_lines` 必须显式为正。
  参考配置以命名常量 `kOcrBatchReferenceMaxRecLines=64`/
  `kOcrBatchReferenceFinalizeReserve=500ms` 存在，库不隐式应用；
  `rec_unit_upper_bound` 初值 0（待 M8-01 实测校准、M8-06 冻结）。
- 报告不变量（计数守恒、唯一 candidate_id、执行序号恰为 {0..N-1} 排列、deferred
  无文本/分数、stop/completion 自洽）由核心自由函数
  `ocr_batch_result_consistent` 固化，会话入口对后端报告强制校验。
- 优先级语义：`resolve_ocr_line_priority` 取相交条目的最高优先级，0 仅表示
  "无相交条目"；负优先级有效（排在未指定行之后），NaN 排最后——`OcrPriorityEntry`
  "任意有限 float" 的契约由此成立。
- 缓存隔离：预算入口首版不读写能力结果缓存（请求类型不设 cache_policy 字段），
  与上文第 5 条一致。

## M8-05 实验结论（2026-10-19）

第 6 条"batching 有退出条件的实验"与第 1 条"单行优化"按 AC8-09 门槛完成实测：
**全部 no-go，本决策与 M8-03 的有界逐行实现不变，未修改任何默认值。**

- 真 batching：能力性不支持——pinned rec 计算图 `Input x 0 1 x` 为 3 维（无样本
  维），pinned ncnn `Extractor::input(blob, Mat)`/`ncnn::Mat`（维度上限 4，最高维
  为通道）执行模型无 batch 轴。真 batch 需带 batch 轴的模型重导出与逐层审查，
  超出"不改 pinned runtime/权重"边界；伪 batch 未测（AC8-09 不计）。
- 横向 packing（4 行拼接）：组级文本匹配 8.3%（钳 320）/20.8%（钳 4096）；
  后者同内容 wall 1.72×、RSS 4.9×。
- 固定宽 pad320：文本一致率 82.3%——动态宽度（保持纵横比、上限钳宽、不 padding）
  的既有设计得到证据支持。
- 宽度分桶 bucket32/64：性能方向无法弥补文本一致率 96.9%/90.6% 的硬伤；padding
  改变 CTC 时间步，输出差异不能压到"文本一致 + 置信度差 ≤1e-4"，机制上不可修复。
- 线程扩展：主矩阵表面收益（threads4 p95 −70%）经同进程 ABBA 顺序对照证伪——
  暖机 serial（p50 32.9 ms/p95 35.7 ms）反而快 12.8–23.5%，为 powersave 冷启动
  顺序伪影；备选方案表"线程/绑核已报告负优化"在本机受控条件下复现。
  `num_threads=1` 默认（DEC-012 位确定性契约）不变。
- 端到端对照：真实权重 dense 帧 338/338 全识别、kCompleted、5 次报告逐位一致。

数字、方法与局限见[实验报告](../benchmarks/ocr-rec-opt-experiment-2026-10.md)；
测量由 Independent-Verification-Agent 独立执行。给 M8-06 的输入：暖机 serial
p95 35.7 ms/行作 `rec_unit_upper_bound` 推导参考（冻结时按参考机重测）；性能
对比必须 ABBA 顺序对照或固定频率。
