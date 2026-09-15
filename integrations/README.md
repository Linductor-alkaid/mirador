# integrations — 参考能力后端（POST-05，`DEC-015`）

本目录承载 Mirador 的**参考能力后端交付包**：在真实模型 runtime 上实现公共
`OcrBackend` / `DetectorBackend` SPI（`DEC-012`），验证 runtime 可适配性并为 M5
评测提供真实能力。

## 边界（强制）

- **默认构建零获取**：整个目录只在 CMake 选项 `MIRADOR_BUILD_INTEGRATIONS=ON` 时
  进入构建。默认构建（含 CI 常规矩阵、最小核心构建）不配置、不下载、不编译、
  不链接任何 runtime；核心发布包不含本目录产物，参考后端不随核心版本号发布。
- **runtime 隔离**：ncnn（当前主选，pinned 20260526）只在 `integrations/` 内出现；
  头文件不含 runtime 类型，架构扫描锁定 `examples/`、`benchmarks/`、`tests/` 与
  `include/`、`src/` 无 runtime 令牌。备选 runtime（ONNX Runtime）切换时替换的
  仅是本目录内的适配层。
- **权重与隐私**：模型一律由使用者显式路径提供，不进仓库、不下载、不缓存；
  图像只在内存中处理，不落盘、不联网（`RULE-10`）。
- **并发边界**：包装层是同步 API；ncnn 的内部线程池只在 forward 调用内使用，
  返回前完成。`num_threads=1` 保证位确定性（缓存正确性要求）。

## 布局

```text
integrations/
  CMakeLists.txt     FetchContent 拉取 pinned ncnn + 锁定校验
  deps.lock.json     依赖锁定（commit、许可证、审计记录指针）
  common/            ncnn 运行时包装（NcnnRuntime：load / run / pack_image）
  common/test/       合成 tiny 模型冒烟（构建时生成模型，权重不入库）
```

## 使用

```bash
# 需要 configure 阶段网络访问（拉取 pinned ncnn）
cmake -S . -B build/integrations -G Ninja -DMIRADOR_BUILD_INTEGRATIONS=ON
cmake --build build/integrations
ctest --test-dir build/integrations -R mirador.integrations
```

规划中的交付物：`integrations/ocr_ppocr`（PP-OCR mobile，M5-03）、
`integrations/detector_yolo`（YOLO 系，M5-04）。真实模型的评测数字按 `DEC-011`
环境与归因口径发布（Core 外层开销与 runtime 耗时分列）。
