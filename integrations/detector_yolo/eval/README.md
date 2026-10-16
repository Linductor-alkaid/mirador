# YOLO 真实模型离线验收

对应 [YR-01…05](../../../docs/plans/yolo-real-model-validation.md)、M5-04 / DEC-015。
合成模型 smoke 继续留在默认 integrations 测试；本目录额外验证真实权重、坐标、
缓存、视觉质量和耗时。工具不下载模型，不随 Core 默认构建，不将图片/权重写入仓库。

## 已验证输入

来源、版本、摘要与许可证见 [assets.lock.json](assets.lock.json)。

- Ultralytics YOLOv5 v7.0 `yolov5n.onnx`：80 类，原始 FP16 `[1,25200,85]`。
- Microsoft OmniParser-v2.0 的固定 revision `icon_detect/model.pt`：本轮 checkpoint
  报告 YOLO11m / 20,030,803 参数，单类 `icon`；不能从目录名推断架构。先用
  Ultralytics 8.3.0 / torch 2.1.2 CPU 导出 ONNX，原输出 `[1,5,8400]`。
- pnnx **20260526 Linux** 与 pinned ncnn 20260526 配对。20260704 转换图曾在
  pinned runtime 中崩溃；准备脚本检查可执行文件摘要，拒绝未经验证的转换器。

模型仅作显式本地评测，权重、外部图片与转换器不在此 PR 中再分发。
评测报告见 [Linux x64 记录](../../../docs/benchmarks/linux-x64-yolo-real-model-2026-10.md)。

## 准备与运行

先按锁文件的来源取得本地文件并核对 SHA-256。Python 评测工具需 numpy、Pillow、
onnx 1.17.0、onnxruntime 1.19.2；UI 导出另需 torch、Ultralytics。使用隔离环境，
不把这些 Python 包加入 Mirador 的构建依赖。

```sh
# 显式启用 runtime 和 CLI；默认配置不取模型、不构建本工具。
cmake --preset release -DMIRADOR_BUILD_INTEGRATIONS=ON -DMIRADOR_BUILD_YOLO_EVAL=ON
cmake --build --preset release -j 4

python integrations/detector_yolo/eval/prepare_model.py \
  --onnx /local/yolov5n.onnx --pnnx /local/pnnx-20260526-linux/pnnx \
  --output build/yolo-real/models/validated_coco --layout rows --classes 80
```

UI checkpoint 的导出（不修改学习参数）：

```python
from ultralytics import YOLO
YOLO('/local/omniparser-v2.pt').export(
    format='onnx', imgsz=640, opset=12, simplify=False,
    dynamic=False, half=False, device='cpu')
```

```sh
python integrations/detector_yolo/eval/prepare_model.py \
  --onnx /local/omniparser-v2.onnx --pnnx /local/pnnx-20260526-linux/pnnx \
  --output build/yolo-real/models/validated_ui --layout xywh_class --classes 1
```

准备脚本把 FP16 常量/权重提升为 FP32；`xywh_class` 只转置输出并插入恒等
objectness=1，得到 `[1,N,5+C]`。两条路径都把输入/输出 blob 命名为 `x/out`。
脚本输出转换日志和 provenance JSON；不会绕过 Backend 或以其他框替代其输出。

输入图片为无注释、8-bit RGB P6，最大 8192 边、64 MiB。可由 Pillow 将调用方本地
图片转换成 `.ppm`。JSON manifest 的完整示例见 [manifest.example.json](manifest.example.json)，
所有模型与图像摘要在运行前校验；路径或导出版本变化时须明确核对并更新对应摘要。
[annotations.json](annotations.json) 是本轮两张定量图片的独立人工 GT，其他图片只作
探索性复核。禁用按钮按可见控件区域标注，模型不证明其可点击性。

```sh
# 一次性检查，张量/编号框/JSON/HTML 只写入调用方指定的 output 目录。
python integrations/detector_yolo/eval/review_real_results.py \
  --manifest /local/manifest.json \
  --executable build/release/integrations/detector_yolo/mirador_yolo_real_eval \
  --output build/yolo-real/review --iterations 1
# 基准须另用至少 11 次采样；单次检查的 p50/p95 字段不作为分位数证据。
```

CLI 也可独立运行：

```sh
build/release/integrations/detector_yolo/mirador_yolo_real_eval \
  /local/model.param /local/model.bin 80 /local/image.ppm \
  build/yolo-real/result 11 0.25 1
```

最后一项为 `contract(0/1)`：真实模型上检查 stride、取消、deadline、工作预算、
类别/输出布局拒绝、confidence=1、session 缓存/刷新、ROI 坐标恢复和 90/180/270
旋转元数据。像素必须已经按 `ImageView` 契约定向；旋转元数据不会旋转输入内存。
`backend_peak_rss_kib` 在附加诊断 runtime 和契约测试之前采样；包括模型、端到端
工作区和当前进程开销，不等于模型权重大小。该指标目前仅在 Linux 报告。

## 对照与判定

C++ 首先调用真实 `YoloDetectorBackend::detect`，输出原始模型输入和模型响应，
Python 使用 ONNX Runtime **CPU 单线程**对相同输入独立 forward，再自行解码、
逆变换和 class-aware NMS。框数量/类别须相同，逐框 IoU ≥0.99、分数误差 ≤0.001。
raw 张量差异另外报告，不因结果框一致就称逐位等价。

此对照验证的是相同输入下的转换、推理和后处理；Mirador 的 area resize / pad=0
与上游训练工具的 bilinear / pad=114 不是同一预处理，不宣称与原工具端到端等价。
少量图片的 precision/recall 仅对相应标注成立，不代表 COCO mAP 或桌面点击成功率。


还可直接核验 UI checkpoint→ONNX，并对比固定方形输入与上游自动矩形预处理：

```sh
python integrations/detector_yolo/eval/compare_upstream.py \
  --checkpoint /local/omniparser-v2.pt --onnx /local/omniparser-v2.onnx \
  --image /local/image.ppm --output build/yolo-real/upstream.json
```

原工具的自动矩形输入与固定 640 方形输入会得到不同候选，报告必须同时注明尺寸；
不能把这种差异算作导出错误。
