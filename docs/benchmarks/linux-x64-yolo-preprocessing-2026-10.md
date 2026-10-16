# YOLO 模型预处理修复复验（2026-10-16）

依据 DEC-024 / YP-01…03，维护者要求修复并接入 Mirage。组合现有 PR #37/#39
后新增显式 `kArea/kBilinear` 和 pad 字节，默认仍 area/pad0；本轮 UI 选择 bilinear114。
Backend 和诊断使用同一输入函数，图像保持内存隐私，只有显式 eval 写张量/标记图。

相同两套真实权重、14 个样本、57 个输出框；bilinear114 路径与独立 ONNX Runtime
同输入对照 14/14，最小框 IoU 0.99999157。与独立 OpenCV INTER_LINEAR/pad114
预处理对照 14 张图，8-bit 像素最大差 1，均值不超过 .00313；不称逐位相同。
原 default area/pad0 与旧公开 letterbox+pack 路径的张量相等由单元测试锁定。

人工表单 6/6 匹配，框 IoU .8441–.9218，中心偏差 .1634–.7509 px，6/6 中心
位于真实目标内；另有 1 个面板候选（precision 6/7）。公交 4/5，左边缘残缺行人
仍漏检，不能把坐标对照通过称召回通过。物理旋转 90° 公交仍零框。

已逐图查看真实表单、Python 官网与几何叠加图：Python 下载按钮重新出现，得分约
.51；主导航仍漏检，几何矩形/圆形仍可能被单类模型标为 icon。这些质量边界保持公开，
不以两张样本选择全局最优阈值，不宣称 disabled Cancel 可点击。

完整框、人工目标重合度/中心偏差、独立输入误差与模型耗时见
[yolo-preprocessing-20261016.json](yolo-preprocessing-20261016.json)。本轮构建/负载与
先前性能基准不同，3 次采样仅用于复现，不用于通用速度或延迟保证。

复现：先按原 real-model 说明准备锁定权重与图片；manifest 的 model 增加
`"preprocessing":"bilinear114"`，运行 `review_real_results.py --iterations 3`；
再用 `compare_preprocessing.py --manifest ... --results ... --output ...` 对比输入。
原样本来源/许可/转换 pin 见 [原验收](linux-x64-yolo-real-model-2026-10.md)。

Release CTest 58/58；同一 Debug 构建同时启用 ASAN/UBSAN，58/58。Sanitizer 构建
关闭 ncnn runtime CPU dispatch/AVX variants，仅作为内存/UB 验证，不作性能比较。

严格 sanitizer 复核：初次可恢复 UBSAN 在旧常量模型的 64×64/stride64 卷积
触发 pinned ncnn x86 SGEMM 成本启发式的 signed overflow；不是 YOLO 坐标变换。
将测试模型改为全局均值池化 + 1×1 零权重卷积，仍生成相同常量 proposals，
不抑制检测器、不修改 ncnn；以 `UBSAN_OPTIONS=halt_on_error=1` 重跑全量 58/58，
无 sanitizer 诊断。该大核/大 stride 的 ncnn 限制仍存在，真实已验权重不含此组合。
