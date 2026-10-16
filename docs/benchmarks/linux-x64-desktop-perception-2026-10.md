# Linux x64 桌面感知质量与性能（2026-10-16）

> 工作项：DP-01…05；DEC-023；[issue #36](https://github.com/Linductor-alkaid/mirador/issues/36)
> 基线：`ae8cf87581c1ebe393a67374d9977a38354d0911`
> 候选：`perf/desktop-perception`（本报告所属 PR）

## 环境与方法

Linux x86_64，Intel Core Ultra 5 225H，GCC 13.3.0，CMake 3.28.3，
C++20 Release `-O3 -DNDEBUG`，无模型/runtime/硬件加速。
两份可执行文件使用完全相同的 `benchmarks/desktop_perception_bench.cpp`，
分别链接基线和候选静态库。每项预热 2 次，采样 15 次；固定 CPU 0（taskset），
真实帧做 3 轮前后交替运行，合成帧另做 1 轮。p50 为排序后中间样本，p95 为
`floor((N-1)*0.95)` 下标。表中取三轮各自分位数的中位数，原始分位数见
[CSV](desktop-perception-2026-10.csv)。没有进行显著性检验，不将结果推广到其他机器。

真实帧来自 Mirage ScreenProvider 的 1000×680 RGB UI 测试画面，ROI
`(55,530,98,27)`；内容 SHA-256：
`a89e1e06c086930996cdb99c3d415a4391ae35b71ade0c32f153e1a5f5663be7`。
外部原图只保留本机，未提交至仓库。默认合成输入为 harness 内确定性 400×240 RGB，
ROI `(160,90,40,28)`，任何机器可复现。变化检测同帧路径比较同一输入，变帧路径
对所有颜色字节取反，确保进入 block diff。描边输入为 300×180 灰度中的
150×80、2px 空心矩形，默认检测器参数。

## 耗时

单位 µs，耗时减少为 `(before-after)/before`。

| 真实帧路径 | 基线 p50 | 候选 p50 | 减少 | 基线 p95 | 候选 p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 目标 E1 验证 | 296247.075 | 208616.907 | 29.58% | 297595.205 | 210189.345 |
| 变化检测：同帧 | 2156.091 | 2153.253 | 0.13%（噪声内） | 2197.919 | 2276.064 |
| 变化检测：变帧 | 7116.863 | 4904.794 | 31.08% | 7258.101 | 5173.416 |
| 2px 描边检测 | 394.383 | 329.331 | 16.49% | 409.057 | 342.006 |

同帧路径没有可证明收益，p95 有约 3.6% 波动；该路径算法不变。
合成输入 E1 p50 54533.506→38218.365 µs（−29.92%），变帧
1225.373→885.558 µs（−27.73%）；单轮结果只作交叉核对。
跟踪仍需约 209 ms，不声明可实时跟踪；逐候选 area resize 与 NCC 仍占主要工作。
未测量 OCR Backend 性能、RSS 或跨平台性能。

## 正确性与视觉复核

- 真实帧 E1 优化前后：NCC=1、PSR=4.5801676025461742、偏移=(0,0)，
  完全一致。默认 PSR 门槛 5 仍拒绝此目标；没有用放宽判定换取耗时下降。
  Gray/RGB/BGRA/NV12、奇数尺寸、分数/边缘 bounds 的差分测试，按旧公共
  crop→完整 fingerprint→NCC 重建整个响应面，精确比较峰值、PSR、偏移。
- 2px 空心矩形默认检测：基线 0 段，候选 8 段（内外边缘各四边）。
  1/2/3/4px、黑白两种极性、奇数尺寸/非连续 stride 均有回归；原有
  抗锯齿、噪声、旋转测试继续通过。
- 原 geometry 截图叠加逐条线段人工视觉复核：基线 75 段、候选 162 段，
  候选恢复空心矩形、圆弧、三角形及折线边缘。文字也可产生线段，
  线段召回不等于几何 proposal 或语义标注 precision，本 PR 不宣称其整体精度提升。
- 原 16×16 小改图，全图 dHash 相似度仍为 1。`thumbnail_size=128`、
  `block_size=4`、`block_diff_threshold=2`、相似阈值 1 时：
  缺省早退→kNone；`fingerprint_early_exit=false`→kPartial，ROI
  `(625,318,63,22)`，面积比例 0.001953125，覆盖可见修改。
  这是阈值化缩略图分块结果，不保证检出所有像素修改。

## 复现

```sh
cmake --preset release -DMIRADOR_WARNINGS_AS_ERRORS=ON
cmake --build --preset release -j 4
ctest --preset release
# 默认合成；固定 11 次，也可由第 2 参数指定外部帧采样数
taskset -c 0 build/release/benchmarks/mirador_bench_desktop_perception
# 本地帧须为无注释、8-bit P6，并包含固定 ROI (55,530,98,27)
taskset -c 0 build/release/benchmarks/mirador_bench_desktop_perception /path/to/frame.ppm 15
```

对照基线时，将本 PR 的相同 harness 复制到基线工作树，使用该工作树的 include
及 Release 静态库编译；不要用历史其他机器数字作前后对照。

## 验证记录

Release warnings-as-errors 全部 53 个 CTest 目标通过（含架构测试）。
ASAN、UBSAN 全部 53 个 CTest 目标通过；ASAN 初轮与 UBSAN 编译并行时，
现有 privacy 用例扫描 `/tmp` 误将编译临时文件当作库写盘，停止编译后的全量重跑通过，
没有压低断言或跳过测试。新增描边、长区域取消、同 dHash 精细比较三项回归
分别链接基线实现验证均失败，候选通过。静态检查结果随 DP-05 记录。
