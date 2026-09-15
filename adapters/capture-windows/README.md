# adapters/capture-windows（Windows GDI 采集适配示例）

M5-05 平台采集适配示例：以 Win32 GDI（`BitBlt` + DIB section）把桌面或窗口客户区
转换为 Mirador `Frame`（RGB8、Frame 持有缓冲、内存内处理、不落盘，`RULE-10`）。

## 边界说明

- **独立适配层**：`mirador::core` 不引用本目录；Win32 类型（`HWND`/`HDC`）不跨入
  公共头（`RULE-02`），对外只暴露 `GdiWindowId`（指针尺寸不透明值）与 Mirador 类型。
- **构建**：`MIRADOR_BUILD_ADAPTERS_CAPTURE_WINDOWS=ON`，仅 Windows 主机可用；
  CI `windows` job 编译验证。本机无 Windows 运行环境，运行时冒烟按 `DEC-011`
  记录补跑条件。
- **同步与取消**：与 X11 适配器同契约——BitBlt 是单次原子调用，`ExecutionContext`
  在其前后轮询取消/deadline；无内部线程（AGENTS.md 并发规则）。
- **kDisplay 变换（DEC-016）**：`window_display_transform` 返回 kOriented→kDisplay
  平移（客户区屏幕原点）；GDI 采集为 1:1 像素、无旋转。桌面窗口对应恒等变换。
- **能力限制（诚实声明）**：GDI BitBlt 对分层窗口（layered）、DRM 保护内容与部分
  硬件加速表面可能得到黑色或陈旧像素；需要这些表面的调用方应改用
  Windows.Graphics.Capture 适配（本示例不覆盖）。DPI 虚拟化下屏幕坐标为虚拟化
  坐标，调用方需以与采集一致的 DPI 上下文解释 `kDisplay`。
