# Linux x64 基准报告：变化检测门控三级短路开销对照（2026-09）

> 状态：Active（M7-03 交付数字）
> 日期：2026-09-22
> 负责人：linductor
> 环境：全部数字按 [DEC-011](../decisions/DEC-011-benchmark-environments.md)
> 采集，**仅对下述机器与构建有效**，不构成跨平台声明。

## 环境

| 项 | 值 |
| --- | --- |
| OS | Ubuntu 24.04 x64（Linux 7.0.0-31），与变化检测基线报告同机 |
| 构建 | release（`cmake --preset release`，-O3/-DNDEBUG），CMake 3.28.3 + Ninja 1.13.2，GCC 13.3.0 |
| 分支 | `feat/m7-03-change-gated-short-circuit`（M7-03 交付点） |
| 方法 | 确定性合成帧；预热 20 + 固定 300 迭代；p50/p95 墙钟（`DEC-011` §2 口径） |

## 结论（M7-03 验收口径：短路路径对 M1 基线无可测回归）

`evaluate_change_gate` 自身开销（对确定的 `ChangeReport` 直接计时，9 个
track 池、三次独立运行、全部三级路径）为 **p50 0.09–0.34 µs**，且 run 间
稳定；相对 M1 `detect_change` 基线（同日同机复测：指纹快速路径 p50
1.63–3.46 ms、分块差分全量路径 p50 ≈ 10.8 ms），门控开销约为全量路径的
0.003%、快速路径的 0.02%。`detect_change + evaluate_change_gate` 合并计时
与 `detect_change` 单独计时的差值全部落在运行噪声内——**三级短路路径均未
引入对 M1 变化检测基线的可测回归**。

## 变化门控（`mirador_bench_change_gate`，1280x720 RGBA8，9 tracks，300 iterations）

| 场景 | 分类 | verify/reuse | detect p50 | detect p95 | gate p50 | gate p95 | detect+gate p50 | detect+gate p95 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| unchanged | kNone | 0/9 | 3465.89 µs | 3508.87 µs | 0.207 µs | 0.216 µs | 3474.74 µs | 3534.81 µs |
| partial-disjoint | kPartial | 0/9 | 10854.83 µs | 10990.41 µs | 0.237 µs | 0.242 µs | 10852.50 µs | 10953.45 µs |
| partial-hit | kPartial | 2/7 | 10839.25 µs | 10966.42 µs | 0.240 µs | 0.244 µs | 10786.40 µs | 10941.45 µs |
| global-rot180 | kGlobal | 9/0 | 10832.69 µs | 10939.81 µs | 0.199 µs | 0.208 µs | 10835.46 µs | 10967.05 µs |

（表为门控实现修复版后的复测 run——2026-09-23 验证员发现头注释"无额外
分配"与 kPartial 临时 ROI 拷贝不符，实现改为逐 ROI 就地转换后数字仍落
原 0.09–0.34 µs 区间；修复前 run 的同表数字仅个别 p50 在噪声内不同。）

口径说明：

- 四个场景各测三列：`detect_change` 单独、`evaluate_change_gate` 单独（消费
  该场景的确定性 `ChangeReport`）、两者合并。场景分类与逐 track 决策计数由
  基准自身断言（分类错级或 verify 计数不符即非零退出）：unchanged 为一级
  全短路（9 reuse、零逐 track 几何计算）、partial-disjoint 为二级逐 track
  判定全不相交（9 reuse）、partial-hit 为 2 verify + 7 reuse（同帧多 track
  独立短路）、global-rot180 为三级全不短路（9 verify）。
- unchanged 快速路径在本机存在 1.6–3.5 ms 双峰（与
  [2026-09-15 基线报告](linux-x64-change-detection-sizes-2026-09.md)
  的 1.64/3.50 ms 一致；global-rot180 的 detect+gate 合并列在个别 run 亦
  受同款 CPU 频率抖动影响，其 p95 始终与 detect 单独 p95 对齐）。因此
  **"无可测回归"以 gate-only 直接测量为主要口径**（0.09–0.34 µs，run 间
  稳定），detect+gate 合并列仅作对照。
- 9 tracks 为 3×3 网格（`max_targets` 默认 64 的小子集）；门控为
  O(tracks × ROIs)，partial 场景为 9 tracks × 1 ROI，每对相交判定为常数
  开销，池规模线性外推不改变量级。

## 同日 M1 基线复测（`mirador_bench_change_detection`，300 iterations）

| 场景 | 分类 | p50 | p95 |
| --- | --- | --- | --- |
| unchanged | kNone | 1632.44 µs | 1693.92 µs |
| partial-change | kPartial | 10804.51 µs | 10870.95 µs |
| rotation-180 | kGlobal | 10799.78 µs | 10887.10 µs |
| dynamic-ignored | kNone | 10846.55 µs | 10941.05 µs |

（2026-09-15 报告同一表格的复测；unchanged p95 与当日发布值 3.50 ms 同量
级，属本机调度抖动区间。）

## 复现命令

```sh
cmake --preset release && cmake --build --preset release
./build/release/benchmarks/mirador_bench_change_detection
./build/release/benchmarks/mirador_bench_change_gate
```

## 限制（`DEC-011` 补跑条件）

- 单机（Linux x64）release 口径；CI runner 数字不采信。物理 Android 设备
  与 Windows 桌面数字缺失，补跑前结论限定 Linux x64。
- 9-track 池为开发冒烟规模；M7-09 合成 harness（A/B/C/D 矩阵、
  `*-static-page`/`*-scroll` 等场景）将按 `DEC-019` 第 5 条对门控路径做
  全规模校准，本报告数字不预支该结论。
