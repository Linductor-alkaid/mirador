# adapters/capture-android（Android MediaProjection + Accessibility 适配示例）

M5-05 平台采集适配示例，两部分：

1. **MediaProjection 投影采集**（`projection_capture`，仅 NDK 构建）：Java 侧持有
   MediaProjection 授权与 VirtualDisplay 生命周期，把投影内容渲染进本适配器
   `AImageReader`（YUV_420_888）的 Surface；C++ 侧 acquire 最新帧并转换为 RGB8
   `Frame`（Frame 持有缓冲、内存内处理、不落盘，`RULE-10`）。
2. **Accessibility 区域转换**（`accessibility_regions` + `accessibility_jni`）：Java 侧
   遍历 AccessibilityNodeInfo 树（AccessibilityService 是平台管道，Mirador 不提供），
   逐节点推送；C++ 侧把边界值转换为 `ExternalRegion`（kDisplay 空间）。JNI 类型只
   出现在 `accessibility_jni.cpp`（`RULE-02`）。

## 边界说明

- **独立适配层**：`mirador::core` 不引用本目录；NDK/JNI 类型不跨入适配器公共头。
- **构建**：`MIRADOR_BUILD_ADAPTERS_CAPTURE_ANDROID=ON`。纯转换部分任意主机可构建
  并有宿主测试（`mirador.adapters.capture_android_accessibility`）；NDK 部分
  （`projection_capture.cpp`、`accessibility_jni.cpp`）仅 `ANDROID` 工具链构建，CI
  `android` job 编译验证。真机运行冒烟按 `DEC-011` 记录补跑条件。
- **kDisplay 变换（DEC-016）**：VirtualDisplay 将物理屏幕缩放进采集缓冲，
  `display_transform`/`projection_display_transform` 返回 kOriented→kDisplay 的纯
  缩放（真实显示尺寸由 Java 侧经 DisplayMetrics 提供，core 不查平台指标）。
  Accessibility 边界值本身就是显示坐标，直接以 kDisplay 证据进入融合。
- **JNI 约定**：sink 由 `createSink`/`destroySink` 管理所有权（Java 持有、负责释放）；
  越界 JNI 调用（空 sink、空数组）静默丢弃；分配失败映射为 Java
  `OutOfMemoryError`，C++ 异常不跨 JNI 边界。
- **编码限制（诚实声明）**：JNI `GetStringUTFChars` 为 modified UTF-8，增补字符以
  代理对形式出现；需要完整 Unicode 保真时 Java 侧应传
  `text.getBytes(StandardCharsets.UTF_8)`（本示例展示简单字符串路径）。
- **能力限制（诚实声明）**：MediaProjection 需要用户逐次授权，适配器不处理授权 UI；
  YUV→RGB 采用 BT.601 studio-swing 逐像素参考实现（确定性、非 SIMD），性能场景
  应由调用方评估 GPU/NEON 路径。
