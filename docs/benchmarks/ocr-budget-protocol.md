# OCR 交互预算评测协议（M8）

> 状态：Proposed
> 日期：2026-10-19
> 负责人：linductor
> 依据：[DEC-025](../decisions/DEC-025-budgeted-ocr-and-partial-results.md)、
> [M8 验收](../plans/m8-budgeted-ocr.md)、DEC-011

本文件定义待执行方法与拟定门槛，不是基准结果。Issue #41 的 12.9–23.4 s 为
下游报告；本项目复测报告须另列环境、原始数据和失败样本。

## 1. 输入与环境冻结

M8-01 必须提交无需下载权重的数据生成器/runner 源码及准确命令。真模型通过显式
路径提供；模型 param/bin、字典、manifest 的 SHA-256、det_side、rec 宽高上限、
有效动态宽度、归一化、max_boxes 和阈值均记录，不能只写“PP-OCRv5”。权重不入仓。

记录 baseline `cb45a10271c53d3b189bb33eef3f692d94feb562` 与 candidate SHA、OS、
CPU 型号/物理逻辑核数、内存、编译器、CMake、Release 编译选项、ncnn 精确 pin
`e54f7b1f88434e1d844ea0551b880a1cfb079ce1`、OpenMP/packing 设置、CPU governor/
频点/降频与后台负载。普通调度为主报告，绑核/OMP_PROC_BIND 对照单列。
两版本在同机同环境交替 AB/BA 执行，避免把升频或热降频当优化收益。

| 场景 ID | 固定要求 | 用途 |
| --- | --- | --- |
| linux-ocr-dense-359-v1 | 1600×1000 RGB8、359 个真实检测候选，至少 32 行可读非空文本并标注优先 16 行；原报告生成器另作复现，图案不可读时只算压力复现 | 对齐下游报告并验证有用输出，二者分列 |
| linux-ocr-dense-512-v1 | 1600×1000 密集图，max_boxes=512，真实 det+rec 链确实输出 512 候选；标注优先 16 行及至少 32 行清晰非空文本 | 最坏候选预算的端到端门槛 |
| linux-ocr-sparse-N-v1 | N=0/1/32/99，固定文本、尺寸和长短行组合；实际 det 数另报 | 完整识别与低密度回归 |
| linux-ocr-long-lines-v1 | 长行、中文/英文/数字混合、不同动态宽度和单次最慢 rec | 校准原子单元与预算余量 |
| linux-ocr-desktop-real-v1 | 受控聊天/浏览器多窗口截图，至少 10 张密集与 10 张稀疏图，人工文字/几何标注 | 真实桌面质量和收益，数据不入仓 |

框数不能由图片中“画了多少行”代替；固定候选框回放可用于隔离 rec 成本，但必须
标为 replay，不得替代 512 框端到端门槛。若构造不出 512 框，门槛保持未完成并
由负责人补充输入。真实数据沿用[离线数据约定](evaluation-scenes.md)，报告只放
场景/数据摘要和脱敏统计，不默认输出 OCR 全文或保存原图。

## 2. 配置矩阵与统计

1. 全量基线：det=1/2/4 × rec=1/2/4，相同 max_boxes/模型/输入；旧完整 API，
   不限时识别，用于质量 oracle 与线程成本。另复核原报告的 1 次暖机、3 次中位，
   此小样本复核不代替后面的 p95。
2. 候选控制：rec=1、det=1 与显式 det=4 各列 K=32/64/128/全量；阅读序、分数、
   调用方优先级分别跑。K 与排序改变识别集合时，单列覆盖差异。
3. 预算配置：优先级模式、K=64 为初值，硬总预算 5,000 ms、收尾预留 500 ms 为
   初值。在 M8-01 根据 det/最长行/收尾测量校准，M8-06 选定并冻结可通过的配置；
   同时记录低预算/过期/取消/预算不足，不只记录成功调用。
4. 可选优化：分别测单行成本消除与真实 batching；未改变模型的逐行路径为控制组。
   检查 batch=1 与各支持批大小、长短混合、pad 比例/最大宽度和内存上限。

每个性能 cell 预热 ≥2 次、测量 ≥20 次；真实图按图分层列 p50/p95/max，聚合结果
不掩盖逐图失败。p50/p95 采用排序后 nearest-rank `ceil(p*n)`，另列样本数。
冷启动（模型加载+首次调用）单列；5 秒热调用不含进程启动/加载，但入口时已开始的
采集/排队成本须由 Mirage 从总 deadline 中扣除，不能重置新的 5 秒。

固定 K、无时间停止的非计时结果重复 3 次应逐位相同；时间停止的完成行数可不同，
必须验证优先序前缀、完成度守恒和不误标，而非强求墙钟决定的行数确定。

## 3. 测量边界与质量对账

主计时从会话调用开始到结构化报告可供调用方使用，含准备/指纹、det、排序/选择、
裁剪/缩放/打包、rec、CTC、坐标恢复与结果组装。Mirage 集成另计其序列化/消费开销，
总和仍受 5 秒预算；不能只报告 ncnn forward。

每次记录以下无内容字段：

| 类别 | 必填指标 |
| --- | --- |
| 耗时 | 总耗时、det/选择/裁剪/预处理/rec forward/解码/恢复收尾，p50/p95/max，超时/错误次数 |
| 工作量 | detected/recognized/reused/deferred、非空且正确文本数、det 触顶、forward 次数、batch 样本数、优先 16 行覆盖 |
| 质量 | 同候选文本一致率、bbox 最大误差、置信度差；人工 GT CER、文本行 recall/precision；partial 覆盖率与识别正确率分列 |
| 内存 | 结果/各自拥有缓冲的核算峰值与限额、进程 RSS 增量、ncnn 工作内存单列 |
| 预算 | 请求 K、排序/优先级摘要、软停止点/原因、原子单元最长耗时、剩余/收尾时间、是否硬超时 |

文本一致性仅比较实际完成的相同候选，不把 deferred 当作空文本正确；完整覆盖率以
全部检测候选为分母，GT 召回以人工标注为分母，两者不能混用。OCR 识别为空的框
计入 completed，但不计入 AC8-02 的“至少 32 行非空且与基线一致”。

512 候选的所有几何与 det 结果逐框对照，选择优先级不得改变 det 排序前的召回。
batching 必须同样对照逐行文本、置信度、行对应关系，不能只报告吞吐。

## 4. 结果判定与可复现交付

M8-06 对 AC8-01…10 逐项填写 PASS/FAIL/未执行，主报告明确：

- 359 与 512 两个端到端场景各 20 次均 ≤5,000 ms、0 硬超时，每次 ≥32 行非空正确
  文本、16 个指定优先行全部完成；其余候选几何与 deferred 原因齐备。
- 稀疏完整配置 p95 回归 ≤5%；文本/几何等价；旧接口与缓存负向无回归。
- 可选优化不满足采用门槛时给出 no-go，对其余交互修复单独判定。

报告附生成器/runner 准确命令、环境、全部配置和逐次无内容 CSV/JSON 摘要、退出码，
缺数据或权重应非零退出，不把 skip 写成通过。实际入口与结果格式（M8-01 交付）：

### 4.1 runner 入口（M8-01，已入仓）

目标：`mirador_bench_ocr_budget`（`benchmarks/ocr_budget_bench.cpp`，仅
`MIRADOR_BUILD_INTEGRATIONS=ON` 预设构建；链接 `mirador_ocr_ppocr` 与
`mirador::fusion`）。场景在内存中确定性生成，不落盘、不入仓；权重经显式路径
提供，缺失时 exit 3。两套路径：

- `--mode baseline`：冻结的 `PpOcrBackend` 全量逐行识别（AC8-01 复现用，不限时，
  `--det-threads/--rec-threads` 参与配置矩阵）。
- `--mode budgeted`（默认）：`PpOcrBudgetedBackend` 经
  `PerceptionSession::run_ocr_budgeted` 执行，主计时从会话入口开始（协议 §3 边界）。

关键参数：`--scenario dense359|dense512|longlines|sparse`（`--sparse-n N`）、
`--scene-dir DIR`（M8-06 可读场景，见下方"当前限制"节）、`--expect-boxes N`
（真实 det 候选数门）、`--max-boxes`、`--k`、`--soft-budget-ms`、
`--finalize-reserve-ms`（<0 关闭；启用时硬 deadline 固定为入口后 5,000 ms）、
`--warmup`（默认 2）、`--iterations`（默认 20）、`--csv PATH`、
`--model-id/--model-revision`（记入行）。det/rec 模型与
charset 分别经 `--det-param/--det-bin/--rec-param/--rec-bin/--charset` 传入。
baseline 模式每调用以 `kRefresh` 执行（不读能力缓存，AC8-01 矩阵口径）。

输出：stdout 每配置两行 `SUMMARY`（调用数与 p50/p95/max，nearest-rank；末次工作量
计数 detected/recognized/deferred/forwards/nonempty/stop/partial/det_limit_hit）；
`--csv` 逐调用记录 total 与 det_canvas/det_pack/det_forward/det_post/select/crop/
rec_pre/rec_forward/rec_decode/finalize 毫秒及无内容计数列，M8-06 起追加
nonempty_matched/mismatched/priority_covered/max_bbox_delta_px 四列（非 scene-dir
运行恒为 0）。退出码：0 通过；2 参数错误；3 缺模型输入或场景文件；4 场景 gate 未达
（如 dense512 未触到 max_boxes 框、一致性与优先覆盖门失败）；5 执行失败。
环境（CPU 型号/频点/governor、ncnn pin、编译选项）由执行者按 §1 记录在报告附页，
runner 本身只固化测量口径。

当前限制：仓内场景为伪字形压力图案（协议 §1 允许的"压力复现"），不可读，不满足
AC8-02 的"至少 32 行非空且与基线一致"质量门槛。M8-06 补充了可读场景入口：
`benchmarks/tools/render_ocr_scenarios.py` 以调用方字体把桌面风格中英文文本渲染为
`scene.rgb`（packed RGB8）+ `scene.tsv`/`lines.tsv`（逐行真值框与 16 个指定优先行
标记，确定性 seed），`mirador_bench_ocr_budget --scene-dir` 加载该场景后 baseline
参照遍与 budgeted 逐调用比对文本/几何，并执行时间/一致性/优先覆盖（dense/longlines）
与全完成/几何 1e-4（sparse）门；`--expect-boxes N` 固定真实 det 候选数。合成模型
冒烟另有夹具约束：常数概率图的合成 det 模型对整幅场景只产生一个全幅候选（如
1600×1000），其裁剪按高度 48 等比缩放后宽度约 77 px——固定输入宽度的合成 rec 卷积
核必须按该宽度生成才能走通（真实 PP-OCR rec 为全卷积动态宽度，无此约束；独立验证
已用匹配内核跑通 baseline 与 budgeted 双模式）。

当前未执行原因：本次范围为决策与计划制定，未启用真实权重和 Mirage 实测。负责人
linductor 在提供受控模型/图像路径及调用链、完成 M8-01 runner 后按此协议补跑。
（M8-06 执行记录：真实权重两轮实测 + 探测边界修订已完成，AC8-01/02/05 证据与
冻结配置见 [linux-x64-ocr-budget-2026-10](linux-x64-ocr-budget-2026-10.md)；
`linux-ocr-desktop-real-v1` 仍未执行，归 M8-07。M8-06 修订后，det 探测在后处理
域上限运行并自截断，自然候选超过调用方 `max_boxes` 的场景可测且 `det_limit_hit`
可见——协议 §2 的 512 候选门槛不再依赖候选数恰好校准到 cap+1。）
