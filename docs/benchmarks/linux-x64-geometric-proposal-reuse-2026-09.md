# Linux x64 基准报告：几何区域 Proposal 跨帧稳定性与缓存增益（2026-09）

> 状态：Active（M6-05 实验探针数字；`M6-06` go/no-go 判定时引用）
> 日期：2026-09-16
> 负责人：linductor
> 触发：`M6-04` 四项 `DEC-017` 合成口径门槛全部 PASS，条件工作项 `M6-05` 触发执行
> 环境：本报告全部数字按 [DEC-011](../decisions/DEC-011-benchmark-environments.md)
> 采集，**仅对下述机器与构建有效**；全部为**合成场景**测量（`RISK-2026-14` 限定），
> `DEC-017` 第 6 条：缓存增益只测量、**不作晋升门槛**。

## 环境

| 项 | 值 |
| --- | --- |
| OS | Ubuntu 24.04 x64（Linux 7.0.0-31-generic） |
| 硬件 | Intel Core Ultra 5 225H，30 GiB 内存 |
| 构建 | release（`cmake --preset release`），CMake 3.28.3 + Ninja，GCC 13.3.0 |
| commit | `feat/m6-proposal-stability-cache` 分支（基线 5b5030f，即 PR #14 合并提交） |
| 方法 | 确定性合成序列（零随机）；内置自检断言，漂移即非零退出 |

入口：`mirador_bench_proposal_reuse`（`benchmarks/geometric_proposal_reuse_bench.cpp`，
链接 `mirador::geometry` + `mirador::image` + `mirador::cache`）。纯测量，无核心
改动、无会话契约（`DEC-017` 实验边界）。

## 实验 1：跨帧 proposal 关联稳定性探针

8 个实体（6 矩形 + 2 圆角）× 6 帧，每帧每实体独立确定性抖动 ±2 px，逐帧
`propose_regions`（默认参数）；相邻帧 proposal 按 tight-ROI IoU ≥ 0.60 贪心
关联（IoU 降序、索引升序决胜，确定性）：

| 帧转移 | 关联 | 率 | 平均配对 IoU |
| --- | --- | --- | --- |
| 0→1 | 8/8 | 1.000 | 0.982 |
| 1→2 | 8/8 | 1.000 | 0.979 |
| 2→3 | 8/8 | 1.000 | 0.986 |
| 3→4 | 8/8 | 1.000 | 0.979 |
| 4→5 | 8/8 | 1.000 | 0.982 |

最差关联率 1.000、最差平均配对 IoU 0.979：±2 px 独立抖动下 proposal 与实体
一一对应且边界稳定，支持"proposal 可作为跨帧稳定空间锚点"的假设方向。注意
本探针为合成边界（整数平移、无边框形变），正式 `temporal_stability` 契约
仍未建立（实验 API，不入会话契约）。

## 实验 2：Geometry Descriptor + Tight ROI 的 VisualIndex 命中对照

场景：8 个尺寸互异实体（60x40 … 200x140；相邻实体部分尺寸对的双边比仍落在
门控窗口内，如 140x90 对 160x100 为 0.875/0.90——几何门控的分辨边界由此
可测）；内部图案按**框相对坐标**绘制（建模静态 UI 随边框刚性移动），指纹取
tight ROI **内缩 3 px** 的内部裁剪（32x32 缩略图）。帧 0–1 建库（每实体一个
entry，附存的 tight bounds 作为几何描述子），帧 2–3 各实体查询（共 16 次/
变体）。查询参数放宽（perceptual ≥ 0.75、template NCC ≥ 0.60、max 8 候选）
以便门控有候选可筛；门控 = 第一个存储 tight bounds 与查询 proposal tight
bounds 双边尺寸比落在 [0.80, 1.25] 的候选。

- **distinct 变体**（每实体独立强对比图案，视觉可分）：查询内容与建库帧
  字节级一致 → 精确层直接命中。
- **ambiguous 变体**（全部实体共享低对比图案、相位随帧推进）：实体间无内容
  区分度，精确层永不命中，区分完全落到感知哈希/模板层——构造 issue #11
  所述"视觉指纹歧义"的最坏情形。

| 变体 | 查询 | 基线 top-1 | wrong | miss | 门控 top-1 | wrong | miss |
| --- | --- | --- | --- | --- | --- | --- | --- |
| distinct | 16 | **16 (1.000)** | 0 | 0 | 16 (1.000) | 0 | 0 |
| ambiguous | 16 | 4 (0.250) | 12 | 0 | **10 (0.625)** | 6 | 0 |

解读：

- **视觉可分时几何门控零代价**：distinct 下基线与门控均 1.000——几何补充
  不损害已健康的视觉路径。
- **视觉歧义时几何门控显著修复**：ambiguous 基线 0.250（接近 1/8 的随机
  水平，候选并列时按 entry id 升序决胜）→ 门控 0.625，wrong 12 → 6；
  放宽的检索阈值保证候选集召回（miss 恒为 0），门控在全部查询中都有候选
  可重排。
- **门控边界（诚实口径）**：残余 6 次 wrong 全部来自几何相近的相邻尺寸对
  （实测为 100x60↔80x50、140x90↔120x80、180x120↔200x140 各 2 帧，双边比
  1.25/1.20、1.17/1.13、0.90/0.86 均在窗口内）——几何只能区分**几何上可分**
  的实体；当视觉与几何同时歧义时门控失效。这正是 issue #11 把
  Geometry Descriptor 定位为"多源匹配依据之一"而非唯一锚点的证据：完整
  方案需要与更强的视觉指纹或语义证据叠加。

## 口径限定与复现

- **测量非承诺**（`DEC-017` 第 6 条）：缓存增益数字不构成晋升门槛；
  `temporal_stability` 正式契约仍未建立，本页探针为实验 API 测量。
- **合成 ≠ 真实**（`RISK-2026-14`）：均匀/带状合成内容是最坏情形构造，
  真实截图的内容分布、抖动谱与遮挡需按
  [evaluation-scenes](evaluation-scenes.md) 离线约定另行评估（数据不入仓）。
- **环境限定**（`DEC-011`）：数字仅对本页四元组有效；CI runner 不采信。
- 关联判定参数：关联 IoU ≥ 0.60；门控尺寸比窗口 [0.80, 1.25]；指纹内缩
  3 px；查询阈值 0.75/0.60——均随入口代码固定，改动需换新场景版本号。
- 复现命令：`cmake --preset release && cmake --build build/release --target
  mirador_bench_proposal_reuse && ./build/release/benchmarks/mirador_bench_proposal_reuse`。
