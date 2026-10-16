# Linux x64 YOLO 真实权重验收（2026-10-16）

> 工作项：[YR-01…05](../plans/yolo-real-model-validation.md)，M5-04 / DEC-015 / RISK-2026-13
> 基线：`ae8cf87581c1ebe393a67374d9977a38354d0911`
> 范围：真实权重的模型转换、运行、后处理、session 消费、逐图视觉复核与性能。

## 结论与边界

两套真实权重已通过完整 Mirador `YoloDetectorBackend`，14 个样本的 **57 个结果框**
与独立 ONNX Runtime 对照全部匹配。对照通过只证明相同输入下适配链路正确，
不等于模型没有漏检或误检，也不等于 Mirage 产品已装配 Detector。

- 通用 YOLOv5n：公交原图人工标注 5 个可见对象，命中 4 个，precision=1、recall=.8；
  漏掉左边缘残缺行人。人物照片给出 2 个主要人物框；物理旋转 90° 的公交图零框，
  说明该模型对该方向不鲁棒，不能把坐标/张量对照通过称作质量通过。
- UI 模型：表单 6 个可见控件区域全部命中，另有 1 个运行状态面板候选，
  precision=6/7=.857、recall=1。禁用按钮也被框出，检测不能判定可点击状态。
- 真实 Python 官网截图有 20 个候选，但漏主导航和下载按钮；真实宿主截图有 8 个
  候选，其中有文字片段与边缘截断区域。终端静态面板、几何图形也会被报为 `icon`。
  这些无完整人工 GT 的图片仅作探索性复核，不计算总体 precision/recall。
- 空白负例：两模型均零框。通用模型对表单与宿主截图均零框，不能替代桌面 UI 模型。
- CPU 成本较高，特别是 UI 模型；本轮不将其默认装入 Mirage，也不改现有预处理。

人工 GT、固定样本摘要见 [annotations.json](../../integrations/detector_yolo/eval/annotations.json)；
仅对公交和表单的可见区域给出定量口径，IoU≥.5、一对一匹配。不是 COCO mAP，
也不是桌面动作成功率。原图、权重、转换器、raw 张量和标记 PNG 均留在本机。

## 实际执行路径与独立参考

C++ 调用 `YoloDetectorBackend::create/detect`，使用其真实 letterbox、归一化、ncnn
forward、解码、坐标恢复和 class-aware NMS。没有用外部预测框替代 Mirador 输出。
C++ 显式诊断工具把实际输入/输出张量写到指定评测目录；Backend 的默认隐私行为不变。

Python 对同一输入以 ONNX Runtime CPU 单线程执行，再独立计算 score、逆 letterbox、
NMS。通过条件：框数量/类别相同，每个框 IoU≥.99、score 误差≤.001。14/14 全通过，
最小匹配 IoU≥.99995；raw 坐标/分数混合张量最大绝对误差≤.00385，不能称逐位等价。

| 模型 | 样本 | Mirador 框数 | 同输入 ONNX 对照 |
| --- | --- | ---: | --- |
| YOLOv5n | bus / zidane | 4 / 2 | 通过 |
| YOLOv5n | bus 非整数缩放 607×809 / 裁切 / 物理旋转 90° | 4 / 5 / 0 | 通过；旋转质量失败 |
| YOLOv5n | blank / form-light / host-crop | 0 / 0 / 0 | 通过 |
| UI icon_detect | form-light / geometry / terminal-dark | 7 / 5 / 2 | 通过 |
| UI icon_detect | public-python / host-crop / blank | 20 / 8 / 0 | 通过 |

源记录：[14 个样本](yolo-real-20261016-final-review.json)、
[11 次采样](yolo-real-20261016-performance.json)、
[阈值扫描](yolo-real-20261016-thresholds.json)。
对两模型各自的正例，还实际检查了 padded stride、预取消、过去 deadline、
工作预算拒绝、错误类别/输出布局拒绝、confidence=1、session 同帧缓存、刷新、
非零 ROI 到全图坐标恢复，以及 90/180/270 旋转元数据下的 oriented 输出。
旋转元数据测试依照 ImageView 的“像素已定向”契约，不是物理旋转不变性测试。

## 模型、转换与发现的问题

来源、摘要、许可证和导出条件在 [assets.lock.json](../../integrations/detector_yolo/eval/assets.lock.json)。

- [YOLOv5 v7.0 官方权重](https://github.com/ultralytics/yolov5/releases/tag/v7.0)：
  `yolov5n.onnx`，80 类，源 FP16 3.80 MiB；提升 FP32 后 ncnn 权重 7.51 MiB。
- [Microsoft OmniParser-v2.0](https://huggingface.co/microsoft/OmniParser-v2.0/tree/f55d0750e5b94db2125ef0b45b0fa4a85ddc59b4/icon_detect)：
  源 checkpoint 38.74 MiB，本轮模型报告 YOLO11m / 20,030,803 参数、单类 `icon`；
  固定版本导出 FP32 ONNX `[1,5,8400]`，转置并插入 objectness=1 后按既有
  `[1,N,5+C]` 契约消费，ncnn 权重 76.54 MiB。没有重新训练或修改学习参数。

发现并固化的转换条件：pnnx **20260704** 生成的 YOLOv5n 图在 pinned ncnn
20260526 中发生 SIGSEGV；换成 pnnx **20260526** 后两模型均稳定。
离线准备脚本检查转换器可执行文件 SHA-256；不把不匹配的工具版本默认为可用。
未改 pinned ncnn，也不将这一组合故障归为已修复的 ncnn 内核问题。

预处理差异已登记 [issue #38](https://github.com/Linductor-alkaid/mirador/issues/38)：
Mirador 固定 area/pad0；对同一原始 ONNX 采用 bilinear/pad114、640 方形输入时，
Python 官网图输出 19 框，下载按钮得分约 .514，Mirador 路径漏掉它。
上游 PyTorch 默认自动矩形输入输出 16 框，也包含下载按钮。
固定 640×640 输入的 PyTorch/ONNX 另行对照，表单 7/7、官网 19/19 匹配，
raw 最大绝对误差分别约 .00147/.00214，确认原 checkpoint→ONNX 导出没有替换检测语义。
两种输入尺寸/预处理不能混用计数；对照详见 [原工具与 ONNX 记录](yolo-real-20261016-native-reference.json)。
因此不能把所有漏检归因模型；后续需分别消融插值、padding，并提供明确的模型预处理配置。
这是冻结 pad0 契约的适用限制，本测试 PR 不静默改变已有调用方的行为。

阈值局部分析：表单将 .25 提高到 .8 可去掉面板，仍保留 6 个目标；公交同样提高到
.8 则只剩 1/5。不存在由这两张图即可证明的统一最优阈值；不据此更改产品默认值。

## 性能环境与数字

Linux x86_64，Intel Core Ultra 5 225H，GCC 13.3.0 Release `-O3 -DNDEBUG`，
pinned ncnn `e54f7b1`，CPU 单线程、无 Vulkan，runtime 保持现有 unpacked 张量设置。
固定 CPU 0，每项预热 2 次、采样 11 次；p50 取中间值，p95 下标
`floor((N-1)*.95)`。单轮观察，无显著性结论，不推广到其他设备。

| 模型 / 输入 | create ms | 完整 detect p50 / p95 ms | 独立 runtime 路径 p50 / p95 ms | backend 阶段峰值 RSS MiB |
| --- | ---: | ---: | ---: | ---: |
| YOLOv5n / bus | 20.22 | 192.11 / 195.95 | 165.32 / 171.59 | 210.45 |
| UI icon_detect / form-light | 165.65 | 1526.37 / 1601.88 | 1522.98 / 1536.07 | 459.61 |

create 包含本次进程内载入与建图，OS 文件缓存未清空，不能称磁盘冷读。
独立 runtime 路径含输入/输出复制，与完整 detect 分开采样，不相减推导纯 kernel 开销。
RSS 在附加诊断 runtime/契约测试之前采样，包含当前进程、模型与工作区；不是权重大小。
14 个质量样本只采样 1 次，不能拿其 p50/p95 字段当分位数证据。

## 门禁与复现

- Release warnings-as-errors + integrations：**57/57 CTest** 通过，含原有四个合成 runtime 冒烟。
- 默认 ASAN、UBSAN：各 **53/53 CTest** 通过。
- 离线 CLI、YOLO adapter、runtime wrapper 额外使用 ASAN+UBSAN 编译，链接 ASAN Core，
  两模型各自的正例/契约检查通过（LeakSanitizer 开启）。pinned ncnn 静态库沿用 Release，
  未插桩；不宣称 ncnn 全库 sanitizer 覆盖。
- 新增 CLI clang-format / clang-tidy（warnings-as-errors）通过；Python 编译检查通过。
- 默认 Core 仍无 runtime/模型/网络依赖；专用 integrations CI 只新增 CLI 构建/静态检查，
  不在 CI 下载权重或将合成冒烟冒充真实模型验收。

完整操作与命令见 [离线验收 README](../../integrations/detector_yolo/eval/README.md)。
本机 `final-review/review.html` 保留逐图粉色预测框与绿色人工目标；网页/宿主原图不提交。
后续缺口 #38、跨平台真实性能和更大桌面语料另行验收，本报告不把它们标记为完成。
