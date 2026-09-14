# 更新日志

本文件格式参考 Keep a Changelog，版本号遵循语义化版本；每个里程碑的发布点与
[实施总计划](docs/plans/mirador-implementation-plan.md)的里程碑索引一一对应。

## [Unreleased]

M1「基础图像与变化检测」（发布点 `v0.1.0-beta.1`）全部工作项已落地。
M2「Backend SPI 与能力结果缓存」（发布点 `v0.1.0-beta.2`）进行中。

### 新增

- M2：Backend SPI 公共契约（`DEC-012`）：`OcrBackend`/`DetectorBackend` 抽象接口、
  `BackendInfo` 能力与实现身份（含 `thread_safe` 显式声明与 `validate` 门控）、
  `TextRegion`/`DetectionRegion` 原始结果、`OcrRequest`/`DetectionRequest`（ROI 与
  空间、`min_confidence`、`max_side`、`backend_params`、`output_space`、`CachePolicy`）
  与 `ExecutionContext` 取消/deadline 通道及其判定辅助。
- M2：`mirador::cache` 能力结果缓存 `CapabilityResultCache`：`RULE-07` 全字段缓存键与
  平台稳定 128 位摘要、字节预算 LRU（淘汰顺序、替换失效、单条目超预算显式
  `kBudgetExceeded`）、请求参数摘要函数；缓存默认预算冻结于 `DEC-008`（帧 4 MiB、
  能力结果 16 MiB）。
- M2：`mirador::image` 有界 `ChangeSignature`（帧尺寸 + 指纹 + 灰度缩略图）与签名版
  `detect_change` 重载：有状态消费者只保留上一帧紧凑签名；与视图版输出位一致。
- M2：`mirador::fusion` 转编译目标（`DEC-013`，链接接口恰为 core+image+cache）与
  `PerceptionSession`：`analyze_change`（首帧 `kFirstFrame` 语义）、`run_ocr`/
  `run_detector`（确定预处理链、Backend 格式门控、按 `output_space` 坐标恢复、
  读/写/强制刷新缓存策略、取消与 deadline 显式报错）；无 runtime 示例
  `perception_session_tour` 纳入默认构建。

- 可选 OpenCV 适配器 `mirador::adapters::opencv`（M1-09，`MIRADOR_BUILD_ADAPTERS_OPENCV`
  默认关闭）：`cv::Mat` → 非拥有 `ImageView` 包装（CV_8UC1/3/4 按 OpenCV 通道语义映射，
  stride 感知，ROI Mat 支持）与 NV12 约定布局（`height + ceil(height/2)` 行 CV_8UC1）
  包装，及深拷贝有界导出 `export_mat`/`export_nv12_mat`。OpenCV 由集成方通过
  `find_package` 提供，不随本项目分发。
- `mirador::image` 编译目标与有界拥有缓冲 `ImageBuffer`：显式字节预算、分配前校验、
  超限返回 `kBudgetExceeded`，NV12 多平面布局遵循冻结的 `DEC-007`。
- 确定性颜色转换 `convert_color`：文档化支持矩阵；BT.601 全范围整数定点系数，
  像素循环无浮点，跨编译器位稳定。
- core `RectI` 像素整数矩形（有效性、包含、相交、相等）与旋转感知裁剪 `crop`。
- 面积重采样 `resize_area`：任意比例整数 box 权重（含放大与非整数比），NV12 仅亮度
  路径输出灰度缩略图。
- dHash 指纹 `dhash_9x8`/`hamming_distance`/`fingerprint_similarity` 与有界组合入口
  `fingerprint()`，stride 无关。
- 分层变化检测 `detect_change`/`ChangeReport`：指纹早退 → 缩略图分块差分 → 8 连通
  变化 ROI 回映帧坐标；忽略区域、帧相似度、变化面积比例、none/partial/global 分类；
  内部缓冲固定预算。
- `mirador::cache` 编译目标与有界 LRU `FrameCache`：payload + 固定开销计入字节预算，
  超预算条目与替换显式报错且缓存状态不变。
- 变化检测基准（不变/局部变化/旋转/忽略区域四场景，p50/p95）与无 runtime 示例
  `change_detection_tour`，均纳入默认构建。

### 变更

- `resize_area` 覆盖权重改为按源行/列预计算，输出位精确不变，720p 检测路径约 3 倍
  提速（本机 release 数字）。

## [0.1.0-alpha] - 2026-09-14

M0「边界与骨架」：公共数据类型冻结、依赖边界锁定与三平台 CI
（[PR #1](https://github.com/Linductor-alkaid/mirador/pull/1)）。

### 新增

- 公共错误模型 `Status` / `Result<T>` / `Result<void>`：九类稳定错误码（无效输入、
  不支持的像素格式、坐标变换、Backend 不可用、Backend 执行失败、超时、取消、缓存损坏、
  预算超限）；异常不穿越公共 API，错误不泄漏 runtime 枚举（`DEC-004`）。
- 公共输入模型 `PixelFormat` / `Rotation` / `ImagePlane` / `ImageView` / `Frame`：
  非拥有视图、NV12 多平面表示（`DEC-007` 草案）、结构性校验 `validate()` 与
  `kMaxImageDimension` 尺寸保护。
- 坐标模型 `CoordinateSpaceId` / `Transform2D`：仿射变换与旋转/缩放/裁剪/镜像/letterbox
  工厂、空间校验的组合与求逆，点/矩形/多边形/线段统一映射（`transform_*` 命名避开
  与 `std::apply` 的 ADL 二义）。
- 架构测试（ctest 标签 `architecture`）：公共头与核心源码的线程设施/第三方库令牌扫描、
  Linux readelf 链接闭包检查、`mirador::core` 链接接口为空的 configure 断言
  （`RULE-01`~`RULE-03`）。
- CMake 模块目标 `mirador::core/image/cache/geometry/fusion/render` 与
  `MIRADOR_BUILD_<MODULE>` 按模块开关；`debug`/`release`/`warnings`/`asan`/`ubsan`/`tsan`
  及 `windows-debug` 构建预设（`DEC-005`）。
- 测试基线：GoogleTest v1.18.0 + ctest 标签机制（`unit` / `property` / `architecture`），
  坐标往返容差矩阵与固定种子属性测试。

### 兼容性影响

- 首个版本，无既有 API 影响。
- 公共头在 GCC、Clang、MSVC 下编译通过；Android NDK arm64-v8a 交叉编译通过。NDK 侧
  仅做 configure/build 验证，设备侧测试按计划在 M5 补跑。
- 测试默认开启（`MIRADOR_BUILD_TESTS=ON`），需要递归检出 googletest 子模块；最小核心
  构建可用 `-DMIRADOR_BUILD_TESTS=OFF`，不获取任何第三方依赖。

### 依赖变化

- 新增 googletest v1.18.0（BSD-3-Clause，submodule + `dependencies.lock.json` 锁定，
  仅测试目标，不进入 `mirador-core` 链接闭包）。审计记录见
  [docs/supply-chain/googletest.md](docs/supply-chain/googletest.md)。
