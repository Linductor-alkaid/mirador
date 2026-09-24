# Linux x64 基准报告：跨帧目标跟踪合成验证 harness 与 A/B/C/D 方法矩阵（2026-09）

> 状态：Active（M7-09 交付数字，`DEC-019` 第 4/5 条口径）
> 日期：2026-09-24
> 负责人：linductor
> 环境：全部数字按 [DEC-011](../decisions/DEC-011-benchmark-environments.md) 采集，
> **仅对下述机器与构建有效**，不构成跨平台声明；**全部为合成口径**
> （`DOD-05`：不宣称真实场景效果；真实截图评估为转正前置，与 `DEC-018`
> 阶段 2 共享采集，见"限制"节）。

## 环境

| 项 | 值 |
| --- | --- |
| OS | Ubuntu 24.04 x64（Linux 7.0.0-31），与 M1/M7-03 基准同机 |
| CPU / 内存 | Intel Core Ultra 5 225H（14 核）/ 30 GiB |
| 构建 | release（`cmake --preset release`，-O3/-DNDEBUG），CMake 3.28.3 + Ninja 1.13.2，GCC 13.3.0 |
| 分支 | `feat/m7-09-synthetic-harness-benchmarks`（M7-09 交付点） |
| 入口 | `benchmarks/mirador_bench_object_tracking`（M7-09 新增，链接 `mirador::fusion`） |
| 方法 | 播种整数哈希合成帧（逐位确定、内存生成、不落盘）；A/B/C/D × 6 场景 × 3 次重复，非计时指标逐位一致（内建断言）；计时为预热 + 固定迭代的墙钟 p50/p95（`DEC-011` §2 口径）；延迟类指标（重捕获）以帧序列计，无墙钟（`RULE-03`） |

## 结论（`DEC-019` 第 5 条门槛逐项判定）

| 门槛初值 | 判定口径 | 测量 | 结论 |
| --- | --- | --- | --- |
| `*-static-page` ID 延续 ≥ 0.95 | 四方法全部 | 1.000（234/234，三方法短路；A 全验证路径亦 1.000） | **通过** |
| `*-scroll`（补偿后）延续 ≥ 0.90 | C/D | 1.000（138/138 静止期 + 36/36 滚动期） | **通过** |
| `*-similar-icons` swap 率 ≤ 0.05 | D（全量方法） | D = 0（0 次交换 / 4 对象）；A/B/C = 0.5（2 次 / 4 对象） | **仅 D 通过** |
| 假阳性延续率 ≤ 0.02 | D | D = 0/1 确认提交 = 0.000；B/C = 2/2 = 1.0；A = 2/116 ≈ 0.017 | **仅 D 严格通过**（A 名义低于 0.02 但 swap 不达标） |
| 静止帧短路相对 M1 基线无可测回归 | 同机同日 M1 复测对照 | detect 单独 p50 3455.7 µs vs 管线前缀（detect+门控+代际触发+sweep）p50 3448.2 µs；M1 同日复测 unchanged p50 3501.6 µs | **通过**（差值在运行噪声内，与 [M7-03 报告](linux-x64-change-gate-2026-09.md) gate-only 0.09–0.34 µs 一致） |
| `kLost` 后静止画面零 Detector 触发 | 全部 24 个 cell | 内建断言：任一 cell 在 `kNone` 帧出现 Detector 调用即非零退出；全部通过（dialog 静态开启期 14 帧、anim 静态期、scroll 静止尾部均零触发） | **通过** |

**核心发现**（方法矩阵隔离出的通道贡献）：

1. **位置门控 + 全局运动补偿是滚动存续的必要条件**（`RISK-2026-17` 门控证据）：
   B vs C 在 `*-scroll` 上延续正确率 0.333 vs 1.000（静止期）、Detector 触发
   120/min vs 0/min、融合层 stable-id 保留率 0.767 vs 0.767（C 无补偿需求时
   与 B 相同——差异在跟踪层：C 六对象零丢失零替换，B 四对象丢失后经
   new-ID 分支替换）。滚动步长 48 px 刻意超出冻结验证 ROI 半径
   （`verification_roi_diagonal_ratio` 1.0 → ±36 px），B 的 E1 结构性够不到
   （设计 §3 预言的似然比反转）；C 经 `estimate_global_shift`（实测置信度
   [0.93, 0.95]）+ `compensate_global_motion` 后 E1 于 (0,−3) px 恢复强匹配。
2. **语义通道是 similar-icons 防交换的唯一被测手段**（`RISK-2026-16` 门控证据）：
   同形异义孪生图标 + 弹出式孪生内容（E1 交叉 NCC 0.712、落在弱带
   [0.6, 0.8)）下，A/B/C 各发生 2 次身份交换（跳上弹出物 + 跳回）；D 经
   `commit_track_evidence` 语义冲突否决（5 次）+ impostor 负模板命中（4 次）
   实现零交换，代价是该对象 2 帧误判丢失后于 2 帧内重捕获（弹出物移除、
   交叉 NCC 0.712 < `impostor_match_threshold` 0.8 不拦真对象）。
3. **E2 结构通道承接主题切换的 E1 失效**：`*-theme-switch` 帧全部 6 track
   E1 = kNone（luma 反转 → 峰值 −1）而 E2 = kConsistent（边缘测度反转不变，
   实测偏差 0.000），六 track 经 `kGenerationSwitch` 场景确认，延续 1.000。
4. **低置信占位路径按设计工作**：`*-partial-anim` 动画窗覆盖 4 帧 →
   E1 kNone → 占位提交 ×4 → `kUncertain`，动画停后 kStrong 恢复，全程
   不丢不换（`uncertain_frame_limit` 5 的恢复窗口内）。
5. **静止帧近零路径无回归**：B/C/D 在 `*-static-page` 全程零验证调用
   （verify=0），池峰值 7,716 B / 1 MiB。

## A/B/C/D 方法口径（harness 内的调用方策略差，池原语同一冻结契约）

| 方法 | 变化门控短路 | 位置门控 | 全局运动补偿 | 语义 |
| --- | --- | --- | --- | --- |
| A 仅外观（E1+E2） | 否（每帧全验证） | 声明透明 | 否 | 否 |
| B 外观+位置门控 | 是 | 是 | 否 | 否 |
| C = B + 补偿 | 是 | 是 | 是（置信门 0.7，见校准表） | 否 |
| D = C + 语义 | 是 | 是 | 是 | 候选语义 + `StableIdTracker::advance` confirmed-associations 直通（M7-06） |

管线为调用方组合帧管线（M7-03~08 冻结形态）：每帧 `detect_change`(M1) →
`StableIdTracker::advance`（DEC-010；D 传 confirmed_associations）→
`advance_generation_for_classification`（M7-07）→（kPartial 帧）
`estimate_global_shift`(M7-04) + `compensate_global_motion`（C/D）→
`evaluate_change_gate`（M7-03）→ kLost track 走重检测：`evaluate_redetection_gate`
（M7-08 门）→ oracle Detector（批调用计数，GT 粗召回）→ `verify_track` 身份
复核 → `commit_track_evidence` → `record_redetection_recapture` /
`record_redetection_failure` / `record_redetection_association` → 活跃 track
`verification_roi` + E2 结构测度 + `verify_track`（M7-05）+ `commit_track_evidence`
（M7-06）→ 新对象 `adopt_track` + `record_structure_baseline` → 对账 →
`sweep_generation_lag`（M7-07）。

## 场景清单（`evaluation-scenes` 场景 ID，合成口径）

| 场景 | 帧数 × 对象 | 分类序列（harness 自断言） | 覆盖点（设计 §8 映射） |
| --- | --- | --- | --- |
| `linux-static-page` | 40 × 6 | 全 kNone | 近零路径、静止期延续、短路回归计时 |
| `linux-scroll` | 30 × 6 | 帧 1–6 kPartial，余 kNone | 补偿与滚动先验（全帧内容 48 px/帧位移，背景水平梯度垂直不变 → 仅对象块变更） |
| `linux-dialog` | 40 × 6（含 1 面板后遮挡对象 + 1 弹窗按钮） | 帧 10/25 kGlobal，余 kNone | 代际切换、遮挡判丢、kLost 后重捕获（静态开启期零触发负向） |
| `linux-theme-switch` | 30 × 6 | 帧 12 kGlobal，余 kNone | E2 承接 E1 失效（全帧 luma 反转，边缘测度不变） |
| `linux-similar-icons` | 30 × 4 + 1 非采纳弹出物 | 帧 12/18 kPartial，余 kNone | swap 与负证据（孪生纹理 + 亮色弹出面板覆盖真对象 62.5%，弹出内容为孪生图标） |
| `linux-partial-anim` | 24 × 4 | 帧 8/12 kPartial，余 kNone | 低置信占位（动画窗覆盖目标 4 帧 → 占位 → kUncertain → 恢复） |

对象 64×32 px，置于 160×90 块对齐"卡片"上（卡片随对象刚性平移，使 M1 块差分
能看到亚块尺寸对象的运动）；E2 描述量由 harness 从帧边缘图测量
（closure = track 边界周长覆盖率、rectangularity = 轴向连通边占比、
edge_support = ≥3 邻接边占比），主题反转不变（自检断言）。

## §8 指标矩阵（3 次重复，非计时指标逐位一致；计时行为末次重复样本）

ID 延续正确率（静止期 / 动态期分开列报；正确 = 对象在帧且其初始 track id 存活于 kTracking/kUncertain）：

| 场景 | A 静/动 | B 静/动 | C 静/动 | D 静/动 |
| --- | --- | --- | --- | --- |
| static-page | 1.000 / – | 1.000 / – | 1.000 / – | 1.000 / – |
| scroll | 0.000 / 0.667 | 0.333 / 0.778 | 1.000 / 1.000 | 1.000 / 1.000 |
| dialog | 1.000 / 1.000 | 1.000 / 1.000 | 1.000 / 1.000 | 1.000 / 1.000 |
| theme-switch | 1.000 / 1.000 | 1.000 / 1.000 | 1.000 / 1.000 | 1.000 / 1.000 |
| similar-icons | 1.000 / 0.786 | 1.000 / 0.786 | 1.000 / 0.786 | 1.000 / 0.929 |
| partial-anim | 1.000 / 1.000 | 1.000 / 1.000 | 1.000 / 1.000 | 1.000 / 1.000 |

swap 率（交换次数 / 对象数）与假阳性延续率（错对象确认提交 / 全部确认提交）：

| 场景 | A swap / fp | B swap / fp | C swap / fp | D swap / fp |
| --- | --- | --- | --- | --- |
| similar-icons | 0.5 / 0.017 (2/116) | 0.5 / 1.0 (2/2) | 0.5 / 1.0 (2/2) | **0.0 / 0.0 (0/1)** |
| 其余五场景 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |

丢失误判率双向（帧数：目标在帧被判丢 / 目标离帧仍被认活）与重捕获（成功/延迟帧）：

| 场景 | A 误判丢/失 | B 误判丢/失 | C 误判丢/失 | D 误判丢/失 | 重捕获成功与延迟（全部方法路径） |
| --- | --- | --- | --- | --- | --- |
| dialog | 0 / 8 | 0 / 8 | 0 / 8 | 0 / 8 | 1/1 成功，延迟 11 帧（面板关闭帧 kGlobal 触发，attempt 数 0） |
| scroll | 6 / 0 | 4 / 0 | 0 / 0 | 0 / 0 | A/B 走 new-ID 分支（assoc 6/4），无重捕获 |
| similar-icons | 0 / 0 | 0 / 0 | 0 / 0 | 2 / 0 | D：1/1 成功，延迟 2 帧 |
| 其余 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | – |

验证路径耗时（`verify_track` 逐调用墙钟 p50/p95，µs）与 Detector 触发频率（次/分钟，60 fps 提示口径）：

| 场景 | A p50/p95 | B p50/p95 | C p50/p95 | D p50/p95 | det/min（A/B/C/D） |
| --- | --- | --- | --- | --- | --- |
| static-page | 194237 / 196306 | –（0 调用） | – | – | 0 / 0 / 0 / 0 |
| scroll | 191121 / 192089 | 142932 / 143648 | 176516 / 192371 | 176577 / 192516 | 120 / 120 / 0 / 0 |
| dialog | 191460 / 192200 | 143986 / 161763 | 143565 / 160719 | 143167 / 160749 | 90 / 90 / 90 / 90 |
| theme-switch | 191617 / 192267 | 143170 / 143334 | 143509 / 143542 | 143367 / 143762 | 0 / 0 / 0 / 0 |
| similar-icons | 191778 / 192476 | 144255 / 144255 | 143084 / 143084 | 143214 / 143527 | 0 / 0 / 0 / 120 |
| partial-anim | 191778 / 192743 | 143008 / 143025 | 143375 / 143733 | 143320 / 143324 | 0 / 0 / 0 / 0 |

目标池内存（`byte_size()` 峰值 / `pool_budget_bytes` 1 MiB，含峰值 track 数）：

| 场景 | A | B | C | D |
| --- | --- | --- | --- | --- |
| static-page | 33,252 B ×6 | 7,716 B ×6 | 同 B | 同 B |
| scroll | 39,960 B ×12 | 13,084 B ×10 | 24,996 B ×6 | 24,996 B ×6 |
| dialog | 32,571 B ×6 | 17,851 B ×6 | 同 B | 同 B |
| theme-switch | 32,868 B ×6 | 14,436 B ×6 | 同 B | 同 B |
| similar-icons | 21,919 B ×4 | 6,303 B ×4 | 同 B | 7,391 B ×4 |
| partial-anim | 21,032 B ×4 | 6,280 B ×4 | 同 B | 同 B |

全程峰值 39,960 B = 预算的 3.8%（A-scroll：6 初始 + 6 替换 track 并存）；
无任何 `kBudgetExceeded`，`byte_size() ≤ pool_budget_bytes` 逐帧断言通过。

融合层补充口径（D 直通证据，`StableIdTracker` retained/regions）：D-scroll
0.967 vs A/B/C 0.767——confirmed-associations 使滚动期 stable id 免于换新；
其余场景 D 与无直通方法一致（0.958–0.975，缺口为遮挡期 id 自然更替）。

## 门槛初值逐项校准（`ObjectTrackerOptions` + shift 参数；库默认全部维持，测量依据如下）

| 参数（默认） | 测量证据 | 决定 |
| --- | --- | --- |
| `max_targets` 64 | 峰值 track 数 12（A-scroll） | 维持 |
| `max_position_history` 32 | 历史无淘汰压力（确认级记录，最多 ~30/track） | 维持 |
| `max_templates` 4 / `max_negative_templates` 4 | 负模板每 track ≤ 1（首拒采集）；正模板更新正常轮换 | 维持 |
| `template_thumb_side` 32 | 同对象峰值 NCC 1.000；孪生区分度 0.712（落在弱带） | 维持 |
| `pool_budget_bytes` 1 MiB | 全程峰值 39,960 B（3.8%） | 维持 |
| `uncertain_frame_limit` 5 | anim：4 帧占位后恢复（streak ≤ 4）；dialog：第 5 次不足提交判丢（行为符合设计边界） | 维持 |
| `max_generation_lag` 1 | sweep 每帧调用，无任何非预期清扫丢失（dialog 丢失全部经 streak 路径） | 维持 |
| `ncc_strong` 0.8 / `ncc_weak` 0.6 | 真匹配峰 1.000；孪生交叉 0.7122 落弱带（kTentative——swap 压力的机制来源）；平坦/杂峰 ≤ 0.64 全部 kNone | 维持 |
| `peak_sidelobe_ratio_min` 5.0 | 真匹配 PSR 全场景 ≥ 6.95（富纹理化调校后）；拒真匹配的平坦响应面 ≤ 3.5。**已知热点**：初版场景纹理实测真匹配 PSR 4.96 < 5.0（同 [M7-08 冒烟记录](../plans/m7-cross-frame-object-tracking.md) PSR≈4.6）——归因于刺激（贫纹理合成 patch）而非阈值，场景纹理富化后复测通过；真实数据转正前置必须复核该裕度 | 维持（附真实数据复核条件） |
| `structure_deviation_tolerance` 0.2 | 主题反转偏差 0.000（应一致）vs 弹窗/遮挡 0.755–16.322（应偏差），干净分离 | 维持 |
| `verification_roi_diagonal_ratio` 1.0 | ROI 半径 ±36 px：48 px 滚动步长刻意越界（B/C 分离的结构来源）；弹出物 +24 px 在界内（swap 机制） | 维持 |
| `verification_work_budget_bytes` 256 MiB | 单次验证规划工作量 ≈ 136.6 MB（5,329 偏移 × 25,632 B），全矩阵零 kBudgetExceeded | 维持 |
| `impostor_match_threshold` 0.8 | 孪生交叉 NCC 0.7122 < 0.8：D 重捕获不被负模板误拦；impostor 命中 4 次（D-similar）全部为真弹出物 | 维持 |
| `min_compensation_confidence` 0.0（库默认维持；**harness 调用方配置 0.7**） | 真滚动估计置信度 [0.93, 0.95]；局部变化帧（弹窗/动画）置信度 [0.46, 0.55] 却伴随非零伪位移——0.0 门下 C/D 曾对局部变化帧平移整池（实测 anim verify 归零异常），0.7 后真滚动照常应用、伪位移全部显式拒绝（`applied=false`，池不动）。库默认是否随 M7-10 上调，留判定记录 | 库默认维持 + 调用方配置 0.7（理由记录于 M7-09 验证记录） |
| `redetect_backoff` 1/60、`redetect_max_attempts` 8 | 最长 episode 3 次尝试内收敛（dialog 首试即成功），未触及 8 次耗尽 | 维持 |
| `max_redetection_records` 64 | 单 cell 最多 1 条中断事件 | 维持 |
| shift：thumbnail 64 / max_shift 16 / 512 KiB | 48 px 滚动 = 4.27 缩略图像素 ≤ 16；恢复 (0,−45) px（量化残差 3 px，在 ROI 半径内）置信度 0.94；零分配超限 | 维持 |

## 内建负向断言（harness 自身，违规即非零退出）

- **kLost 后静止画面零 Detector 触发**：24 cell × 全帧计数，kNone 帧 Detector
  调用数必须为 0（`evaluate_redetection_gate` kHoldStaticFrame 路径的实际效果）。
- **池字节预算**（`RULE-06`）：每帧断言 `byte_size() ≤ pool_budget_bytes`。
- **逐位确定性**（`RULE-03`）：同配置重复运行全部非计时指标（含重捕获延迟的
  帧计数、决策/对账计数器、池峰值）逐位一致；墙钟仅出现在计时列且被显式
  排除出摘要。本次 3 次重复全部一致。
- **场景分类自断言**：每帧 `ChangeReport` 分类与场景规格比对，渲染漂移即失败。
- **自检**：背景 y 不变性（滚动平移场假设）、主题反转的 E2 基线不变、
  滚动位移可恢复（|dŷ+48| ≤ 12 px）。

## 静止帧短路回归对照（同机同日）

| 口径 | p50 | p95 |
| --- | --- | --- |
| `detect_change` 单独（kNone 快路径，6 track 池） | 3455.7 µs | 3494.9 µs |
| detect + `evaluate_change_gate` + 代际触发 + sweep（完整前缀） | 3448.2 µs | 3489.5 µs |
| M1 同日复测（`mirador_bench_change_detection` unchanged） | 3501.6 µs | 3569.9 µs |

管线前缀与 detect 单独差值在噪声内，且不高于 M1 同日基线——**静止帧短路路径
无可测回归**（与 M7-03 发布的 gate-only 0.09–0.34 µs 一致）。

## 复现命令

```sh
cmake --preset release && cmake --build --preset release
./build/release/benchmarks/mirador_bench_object_tracking 3   # 3 次重复 + 确定性断言
./build/release/benchmarks/mirador_bench_change_detection    # M1 同日基线
```

## 限制（`DEC-011` 补跑条件与 `DOD-05`）

- 单机（Linux x64）release 口径；CI runner 数字不采信。物理 Android 设备与
  Windows 桌面缺失，补跑前结论限定 Linux x64。
- **全部为合成口径**：场景、纹理、遮挡与运动均为播种合成，E2 结构测度由
  harness 测量而非真实 `propose_regions` 输出；oracle Detector 按 GT 粗召回。
  真实截图评估（`RISK-2026-14` / `DEC-018` 阶段 2 共享 `~/mirador-eval/` 采集）
  是 `DEC-019` 转正前置，**不在本项**；M7-10 判定必须显式记录"仅有合成证据"。
- `peak_sidelobe_ratio_min` 5.0 的真匹配裕度依赖纹理丰富度（贫纹理合成 patch
  实测 4.96 被拒）——真实数据校准时优先复核。
- A 方法的"仅外观"在决策层隔离（无短路、门控透明），E1 搜索仍受冻结验证 ROI
  机械约束（契约无全帧搜索面）；A 的延续优势场景（如大位移且纹理唯一）不在
  本矩阵覆盖内。
- swap 率分母为场景对象数（4），场景长度 30 帧；门槛 ≤ 0.05 的量纲为
  "每对象交换次数"，随场景定义冻结（换场景即换 ID 后缀，`evaluation-scenes`
  约定）。
