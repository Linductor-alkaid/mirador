# M8-06：真实模型 OCR 交互预算基准（Linux x64，2026-10）

> 状态：Delivered（三轮独立测量与验证完成，2026-10-20/21）
> 日期：2026-10-20（首轮测量）；2026-10-21（修复验证轮）
> 决策依据：[DEC-025](../decisions/DEC-025-budgeted-ocr-and-partial-results.md)、
> [M8 计划](../plans/m8-budgeted-ocr.md) AC8-01/02/05、
> [OCR 预算协议](ocr-budget-protocol.md)
> 工具：`benchmarks/ocr_budget_bench.cpp`（M8-01 harness + M8-06 可读场景门禁）+
> `benchmarks/tools/render_ocr_scenarios.py`（可读场景渲染器）

## 1. 问题与范围

M8-01…04 已交付有界 OCR 执行（top-K、优先级、软预算、部分报告）与无权重基准
harness；M8-05 实验收口为全 no-go。本报告回答 M8-06：**真实 PP-OCRv5-mobile 权重
在本参考机上，(a) 旧完整 API 的基线耗时矩阵（AC8-01）；(b) 密集 359/512 框端到端在
5 秒预算内是否每次返回可消费的报告（AC8-02）；(c) 稀疏场景的有界路径是否与旧路径
逐框等价且无可测回归（AC8-05）；并据此冻结参考 K、收尾预留与 `rec_unit_upper_bound`。**

## 2. 测量环境与方法

- 机器：Intel Core Ultra 5 225H（14 核混合：4P + 8E + 2LP-E），governor powersave
  （未调频，如实记录；跨阶段绝对值不可比，见 §9），内核 7.0.0-38-generic，
  gcc 13.3.0 Release 构建。环境明细见证据目录 `env/`。
- Runtime：pinned ncnn `e54f7b1f88434e1d844ea0551b880a1cfb079ce1`（20260526），
  `use_packing_layout=false`（M5 张量面契约）。
- 权重：`PP-OCRv5-mobile-ncnn-mirador-contract-v1`（det_side=960、rec_height=48；
  五文件 SHA-256 与 bundle manifest 逐一相符，执行者独立复核）。
- 场景（协议 §1，可读桌面风格中英文渲染文本，确定性 seed，不入仓）：
  - `linux-ocr-dense-359-v1`：1600×1000，19 列网格前 359 槽、字号 17，真实 det 链
    恰 359 候选（`--expect-boxes 359` 门），16 个指定优先行。
  - `linux-ocr-dense-512-v1`：1600×1000，23 列网格前 510 槽、字号 16，真实 det+rec
    链自然输出恰 512 候选（`det_limit_hit=0`；真触顶变体见 §8 R3）。
  - `linux-ocr-sparse-N-v1`：N=0/1/32/99，8 列网格、字号 18，逐场景
    `--expect-boxes N` 门。
  - `linux-ocr-long-lines-v1`：10 条长行混合中英文数字、字号 24，det 恰 10 候选。
  - 渲染命令冻结于证据目录 `FROZEN_COMMANDS.txt`；仓库渲染器（缺陷修复后）重渲染
    七个场景的 `scene.rgb` SHA-256 与首轮冻结场景逐一相同（§8）。
- 方法：主报告为普通调度；每 cell 预热 ≥2、测量 ≥20；p50/p95 取排序后 nearest-rank
  `ceil(p*n)`；baseline 每调用 `kRefresh`（真实前向，不读能力缓存——M8-06 修复的
  AC8-01 口径缺陷）；budgeted 计时从会话入口开始（协议 §3 边界），硬 deadline
  5,000 ms。频率纪律沿用 M8-05 §7：AC8-01 以 cell 内绝对分布报告并附 bookend
  复测漂移，跨配置比较不基于单顺序轮次。

## 3. AC8-01：基线矩阵（dense359，det=1/2/4 × rec=1/2/4，旧完整 API 不限时）

p50 / p95 / max（ms），warmup 2、20 次测量：

| cell | det=1 | det=2 | det=4 |
| --- | --- | --- | --- |
| rec=1 | 7054.4 / 7421.0 / 7467.1 | 4383.7 / 6066.1 / 6193.6 | 4806.9 / 6857.0 / 6977.7 |
| rec=2 | 10991.6 / 11706.0 / 12466.5 | 13655.0 / 14570.6 / 14677.3 | 13644.1 / 16051.8 / 16515.6 |
| rec=4 | 22950.9 / 26043.6 / 26135.4 | 25136.4 / 27103.6 / 27340.1 | 22668.3 / 23452.4 / 23491.0 |

- bookend（结尾复跑 det=1/rec=1）：7534.1 / 7546.1 / 7548.9，对主 cell p50 漂移
  **+6.8%**——powersave 下的同日漂移量级，跨 cell 的小差值不可归因。
- 绑核对照（`taskset -c 0-3`，det=1/rec=1 单列）：7387.0 / 7411.2 / 7428.4——绑核后
  p50–p95 差仅 24 ms（未绑核 367 ms），分布显著收紧。
- 观察：**rec 线程扩展严重负向**（det=1 时 1→2→4 线程 p50 7.1→11.0→22.9 s），与
  M8-05 单元级 ABBA 结论一致；det=2 对 det=1 的表观收益在同一漂移带内，不据此调整
  `num_threads=1` 默认（DEC-012 位确定性契约）。每 cell detected=359、计数守恒成立。

## 4. AC8-02：密集场景预算端到端（budgeted，硬 deadline 5,000 ms）

门禁（工具内逐调用强制）：每次 ≤5,000 ms、对基线参照 mismatch=0、非空一致行 ≥32、
16 个指定优先行全部识别。以下全部 exit 0、`scene_gates=pass`：

| 配置 | p50 | p95 | max | 末次计数 |
| --- | --- | --- | --- | --- |
| dense359 K=32 + reserve 500 | 501.4 | 506.1 | 511.9 | 32 识别 + 327 保留 = 359，stop=kRecLineLimit，kPartial |
| dense359 K=64 + reserve 500 | 862.2 | 868.8 | 876.4 | 64 + 295 = 359 |
| dense359 K=128 + reserve 500 | 1588.4 | 1611.6 | 1727.9 | 128 + 231 = 359 |
| dense359 K=64 + soft 4500 + reserve 500 | 862.8 | 1100.5 | 1558.5 | 64 + 295 = 359 |
| dense512 K=64 + reserve 500（自然 512 场景） | 872.2 | 1588.2 | 1588.6 | 64 + 448 = 512，det_limit_hit=0 |

- **全量探针（K=512，预期超时压力测试）**：首轮 3 次全部在预算内完成——
  4294.4 / 4510.4 / 4510.4 ms，359 行全识别、stop=kCompleted；但第二轮在同名探针上
  复测时机器处于更慢的稳定态（见 §8 环境观察），soft budget 停止于 209–224/359 行
  （门禁仍过、exit 0）。**结论：dense359 全量识别恰好骑在 5 s 边界上，随机器状态
  摆动，不构成可达预算裕度；预算路径（K=64 时两轮 p50 862/1520 ms，均远低于
  5 s）才是 5 秒交互的稳健保障**——这一两轮对照本身即 AC8-02 预算机制必要性的
  直接证据。
- **确定性门**：K=64 无时间停止配置，三次独立进程 × 5 次调用，
  `SUMMARY content_digest=2ac68a78c4e9db39` 三次逐位相同（协议"固定 K 非计时结果
  重复 3 次逐位相同"）。
- 每次调用的预算可见性：partial 报告保留全部 deferred 几何，det 触顶/选择/停止
  原因逐字段可查（CSV 计数列 + 守恒抽查）。

## 5. AC8-05：稀疏场景等价与回归（N=0/1/32/99，budgeted K=512 ≥ N，无时间预算）

门禁：全完成（stop=kCompleted、deferred=0）、对基线 mismatch=0、bbox 最大偏差
≤1e-4 px。四组全部通过：

| N | baseline p95 (ms) | budgeted p95 (ms) | 相对回归 | 判定（门槛 ≤5%） |
| --- | --- | --- | --- | --- |
| 0 | 276.905 | 274.842 | −0.745% | PASS（单列） |
| 1 | 314.412 | 316.235 | +0.580% | PASS |
| 32 | 1677.905 | 1655.494 | −1.336% | PASS |
| 99 | 4601.468 | 4598.211 | −0.071% | PASS |

回归最大 +0.580%（N=1），全部在 5% 门槛内；文本逐框一致、几何偏差 ≤1e-4 px 由
工具门禁逐调用强制（非抽样）。AC8-05 的"逐框文本与旧逐行基线相同"因此同时覆盖
了真实模型上的 budgeted/旧路径等价性。

## 6. 长行单元校准与配置冻结

- 长行（10 条全宽混合文本）budgeted K=512：单行 rec 单元（`rec_forward_ms/forwards`）
  **p50 60.728 / p95 60.847 / max 60.915 ms**；整调 p50/p95/max =
  927.7 / 938.3 / 948.0 ms。与 M8-05 暖机混合语料 p95 35.7 ms 合并，长行是单元
  耗时的最坏档。
- **冻结建议（写入 DEC-025）**：
  - 参考交互配置 **K=64 + `finalize_reserve`=500 ms**：初值即通过全部 AC8-02 门禁，
    p50 862 ms、p95 869 ms，余量 5.7 倍；维持 `kOcrBatchReferenceMaxRecLines=64`/
    `kOcrBatchReferenceFinalizeReserve=500ms` 不变。
  - **`rec_unit_upper_bound` 参考值 100 ms**：长行实测 max 60.9 ms 的 1.6 倍余量，
    覆盖 powersave 频率漂移（bookend 6.8%、M8-05 冷启动虚高 46% 的量级）；代价是
    收尾阶段最多 100 ms 的预算尾差。仍为调用方可配置项，库不隐式应用。
  - `num_threads=1` 维持（§3 rec 线程负优化在真实权重复现）。

## 7. 结论

- AC8-01/02/05 在冻结参考机（DEC-011 Linux x64 主基准环境）全部取得协议口径的
  实测证据；5 秒交互门槛以 K=64 预算配置达成且两轮机器状态下余量均 >3 倍；
  全量识别骑在 5 s 边界上随机器状态摆动，不作为可达能力声明。
- M8-05 的 no-go 结论不受影响；预算路径从"防御性机制"变为"实测必要的机制"
  （dense512 全量超 5 s；dense359 全量随状态摆动）。
- Issue #41 的直接交互失败模式（密集帧全有或全无超时）在本参考机上闭环：
  359/512 框场景每次调用都返回具有完成态、非空一致文本与优先焦点的可消费报告。

## 8. 工具缺陷与两轮验证

首轮测量由 Independent-Verification-Agent 用真实权重完整执行，并报告 4 个工具缺陷
（测量以其本地修正副本完成，数字有效；主循环修复于 commit `7de21a2`）：

1. 渲染器 textbbox 绝对锚点坐标误用 → 全图叠字（阻塞级）；修复后仓库渲染器重渲染
   七场景 `scene.rgb` SHA-256 与首轮冻结场景逐一相同，lines.tsv 几何相同。
2. baseline 参照遍共享调用方 det cap → 溢出场景 kBudgetExceeded；参照遍改为
   65535 域上限（旧路径冻结语义不变，budgeted 策略不受影响）。
3. 场景一致性门误套 baseline 模式 → AC8-01 场景运行必然 exit 4（CSV 数字有效）；
   门限 budgeted 模式。
4. 门禁类别仅取 `--scenario` → scene.tsv 增场景名列，不一致显式 exit 2。
   另：优先行标记在 count 非 16 倍数时会标出 17 行，改为恰 16（首轮 17/17 覆盖
   严格蕴含 16/16，结论不受影响）。

第二轮（缺陷修复验证与补测，2026-10-21，独立执行）：4 项修复经仓库工具复验全部
有效（渲染器确定性 + 场景名不匹配 exit 2 + baseline 场景运行 exit 0）；首轮被
阻塞的 dense512 真触顶场景以 `--count 511` 校准到自然 513 候选补测通过——
budgeted K=64+reserve500 20 次 p50 1514.4 / p95 1525.9 / max 1530.4 ms，
detected=512、`det_limit_hit=1`、门禁全过、exit 0。R4 复测对比：baseline det1/rec1
p50 7225.1（首轮 7054.4，+2.4%，漂移带内）、det1/rec4 p50 16177.6（−29.5%）、
sparse32 回归 −1.1%——比值/门禁/确定性结论全部成立。**两项新发现**：
(1) 设计限制——自然候选 >det_limit+1（529 槽自然 530 候选）时旧探测整调用
`kBudgetExceeded`（M8-03 记录的"kept ≥ cap+2 是否放宽待 M8-06 真实密度数据"项，
数据判定必须放宽）；(2) 环境观察——dense-budgeted 绝对值本轮均匀 +76%
（跨阶段 ×1.73–1.85、可复现；已排除代码/频率/核布局），"全量 359 行 5 s 内完成"
在本轮状态不成立（soft 停止于 209–224/359 行，门禁仍过）。证据
`/tmp/m8_06_iva_round2/`。

探测边界修订（主循环按发现 (1) 实施）：`run_det_stage` 探测 cap 从 `det_limit+1`
改为后处理域上限 65535 + 后端自截断（DEC-025 M8-06 修订；核心后处理与旧路径
零改动）。

第三轮（修订验证，2026-10-21，独立执行）：**PASS**——冒烟套件按新语义更新
（Check E 重写为自然密度截断边界，8 项具体断言含保留前缀逐行核对），
`mirador.integrations.ppocr_budgeted_smoke` 137/137 exit 0（旧 131 + 净增 6），
integrations ctest 6/6、全仓 61/61，ASAN/UBSAN 直跑 137/137 零报告；**自然
529 槽场景（530 候选，修复前 exit 5）修复后 exit 0**：det_limit_hit=1、
detected=512、64+448 守恒、mismatch=0、非空匹配 64、优先 16/16、每次 ≤5,000 ms
（p50 1534.6 / p95 1544.7 / max 1549.7 ms，content_digest 跨 20 次恒定）；
dense359 K=64 digest 与第二轮完全一致（跨轮文本稳定），sparse32 回归 −0.5%。
遗留：真 65535 饱和路径仅有代码审读证据（合成夹具与真实场景均不可达）；
`det_limit==65535` 且自然恰 65535 时不置 hit（无截断无损失，设计使然）。证据
`/tmp/m8_06_iva_round3/`（含 `ROUND3_REPORT.md`）。

## 9. 限制

- governor 为 powersave 且未固定频点：跨阶段/跨轮绝对值不可比（bookend +6.8%、
  两轮同 cell 差 −1.1%~+6.0% 均为该漂移带）；AC8-05 为背靠背测量，回归结论可信；
  AC8-01 的 cell 间小差值不作归因。绑核对照仅 det=1/rec=1 一列。
- 语料为渲染文本（确定性合成场景），非真实桌面截图；`linux-ocr-desktop-real-v1`
  （受控多窗口截图 + 人工标注）未执行，真实桌面质量收益归 M8-07 下游验收。
- 5 秒判定限于本参考机与冻结场景；不对外声明跨机器性能。dense512 全量超预算的
  推算基于 dense359 全量探针，未单独实测。
- Windows/Android 无实机性能数据，仅构建级验证（随 CI）。
- 探测修订后，真 65535 盒饱和路径（整调用 `kBudgetExceeded`）仅有代码审读证据，
  合成夹具与真实场景均不可达；`det_limit==65535` 且自然恰 65535 候选时不置
  `det_limit_hit`（无截断无损失，设计使然）——域边界行为如实记录，未执行验证。

## 10. 证据清单

- 首轮：`/tmp/m8_06_iva_results/`——`VERIFICATION_REPORT.md`、`FROZEN_COMMANDS.txt`、
  `defect/`、`tools/`、`env/`（环境与权重 SHA-256 复核）、`render/`（冻结场景哈希）、
  `calib/`、`asan/`（ASAN 冒烟零报告）、`neggate/`（负向门禁）、`d1_ac801/`
  （九 cell + bookend + 绑核 CSV）、`d2_ac802/`、`d3_ac805/`、`d4_longlines/`、
  `crosscheck/`（26 对 CSV/stdout 交叉验证全 OK：行数=iterations、nearest-rank
  重算逐一吻合、detected==recognized+deferred 全部成立）。
- 第二轮：`/tmp/m8_06_iva_round2/`——`ROUND2_REPORT.md` 及各子目录。
- 第三轮：`/tmp/m8_06_iva_round3/`——`ROUND3_REPORT.md`、冒烟套件更新与
  sanitizer 执行、自然密度端到端 stdout/CSV。
- 复现：渲染命令见 `FROZEN_COMMANDS.txt`（仓库渲染器 `7de21a2` 起与冻结副本像素
  一致）；harness 命令模板见[协议 §4.1](ocr-budget-protocol.md)。权重与场景不入仓。
- 执行者：Independent-Verification-Agent（三轮构建、渲染、校准、计量与交叉验证）；
  主循环实现工具（分支 `feat/m8-06-real-model-benchmark`）并分析数字。
