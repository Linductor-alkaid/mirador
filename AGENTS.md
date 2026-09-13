# Mirador 项目协作约定

## 适用范围

本文件适用于 Mirador 仓库中的全部自研代码、测试、文档与构建配置。`third_party/` 与
`integrations/` 中的上游代码和外部适配遵循其自身约定；除非任务明确要求升级或修复依赖，
否则不要修改其中的代码。本文件是仓库级最高强制约束，与[项目管理与工程规范](docs/project/project-standards.md)
（下称"工程规范"）配套使用：本文件定义底线，工程规范定义完整流程、模板与证据要求。
[设计文档](docs/design/mirador-development-design.md)是系统应如何工作的权威来源。

## 项目管理与文档规范

所有计划、里程碑、设计、决策、验证证据和文档变更必须遵循工程规范第 1-8 节。开始非平凡
变更前，必须确认所属计划工作项、相关设计和决策依据；完成时必须同步更新任务状态、测试
结果、验收证据以及受影响文档。环境限制导致的未执行验证不得标记为完成，必须记录原因、
负责人和补跑条件。

## 产品目标

Mirador 是面向 Android、Linux 与 Windows 终端的轻量级 C++ 视觉基础设施库，把终端画面转换为
稳定、可查询、可复用的视觉事实：判断画面是否变化、按需执行 OCR/检测/几何能力、融合多源
区域证据，并在画面未显著变化时以近零成本复用结果。核心闭环为：

`提交帧 -> 变化检测与缓存判断 -> 按需 Backend 能力执行 -> 证据融合与稳定 ID -> SemanticSnapshot/SoM 输出与结果复用`

实现必须保持 Core 与平台、模型 runtime、Agent 逻辑解耦：模型 runtime（ncnn、ONNX Runtime、
MNN、TensorRT 等）、平台采集、Accessibility 数据、VLM 调用与动作执行只能由调用方或独立适配层
提供，不得渗入 `mirador-core`。优先围绕以下接口形成稳定边界：

- `ImageView` / `Frame`：平台无关的非拥有图像输入视图与帧上下文（格式、步长、方向、生命周期）。
- `OcrBackend` / `DetectorBackend`（SPI）：调用方注入的已执行能力后端，含能力查询与实现身份。
- `Status` / `Result<T>`：不依赖异常与 runtime 错误枚举的错误模型。
- `SemanticSnapshot` / `VisualRegion`：带 `stable_id` 与 `generation` 的融合输出，用于陈旧性校验。
- `PerceptionSession`：按图像源组织变化检测、缓存命名空间与稳定 ID 跟踪的感知入口。

## 同步 API 是强制并发边界

Mirador 核心不绑定任何并发框架，同步 API 是能力边界的基准。以下规则是强制要求：

1. 核心不得创建线程、线程池或定时器，不得使用 `std::thread`、`std::jthread`、`std::async`、
   自建线程池、私有定时调度器、detached worker 或 fire-and-forget 工作，不得隐藏后台轮询，
   不得要求全局单例。
2. 异步执行、线程池、任务优先级、超时与实时调度由调用方负责；公共 API 不得暴露 future、
   executor、协程 ABI 或任何调度框架类型。Mira 等上层系统在自己的集成层完成调度。
3. Backend 实现允许在内部使用模型 runtime 自带的异步能力，但必须在同步边界返回前完成；
   Backend 是否线程安全必须由 `BackendInfo` 或能力标记显式声明。
4. 取消与 deadline 只通过轻量 `ExecutionContext`（`is_cancelled` 回调 + 可选 deadline）传递；
   长任务和循环必须定期检查取消状态。超时与取消必须转化为明确的 `Status`，不得静默吞掉。
5. 同一 `PerceptionSession` 默认不允许并发修改；已发布的不可变快照可以被并发读取；跨 session
   共享缓存必须由调用方显式传入。共享可变状态必须有明确所有权。
6. 若确需修改"核心无内部并发设施"这一边界，必须先按工程规范建立决策记录，同步本文件、
   设计文档与架构测试后方可实施；不得以适配、性能或便利为由静默引入。

## Runtime 与依赖边界

Mirador 的核心定位是视觉能力库，不是推理运行时。以下规则是强制要求：

1. `mirador-core` 只依赖 C++20 标准库；不链接 ncnn、ONNX Runtime、MNN、TensorRT、OpenCV、
   ELSED 或任何模型/渲染依赖，不加载或执行神经网络模型，不管理模型权重，不发起网络请求。
2. OpenCV、ELSED、字体渲染等只能作为对应可选模块的实现依赖，通过 CMake 选项启用且默认关闭，
   不进入最小核心构建，不出现在公共头文件中；`cv::Mat` 等第三方类型只出现在 `adapters/`。
3. Backend 由调用方注入；核心不下载模型，不假定模型来自特定供应商。模型权重不进仓库，
   示例通过用户显式提供的路径运行。
4. 随 Mirador 分发的每个可选依赖必须 pin 到精确版本并登记来源与许可证（工程规范第 9 节）；
   `THIRD_PARTY_NOTICES` 覆盖所有随二进制或源码分发的依赖。ELSED 等引入前必须完成许可证审查。
5. 具体 runtime 的 Backend 示例只允许存在于独立仓库或默认构建不获取的 `integrations/`，
   并在适配文档中说明许可证与审查事项。
6. 隐私默认：只在内存中处理图像，不记录原始帧，不写入磁盘，不联网；持久化能力必须由调用方
   显式启用并提供存储位置；日志不得默认输出 OCR 全文或图像内容。
7. 确需突破上述任一边界时，必须先建立决策记录并更新架构测试；只写"需要依赖"不构成有效理由，
   必须说明缺口、备选方案与影响。

## 会话与状态模型

- 核心算法对象尽量无状态或具有明确实例状态；`PerceptionSession` 保存某个图像源的上一帧指纹、
  快照、稳定 ID 跟踪器和缓存命名空间，不同窗口、摄像头或设备使用不同 session。
- 输出分两层：保留各自语义字段的原始能力结果（`TextRegion`、`DetectionRegion`、`LineSegment`），
  与融合后的 `SemanticSnapshot`；上层可只取其一。
- 快照一经发布即不可变；SoM 与上层动作必须携带 `generation`，内容或位置大幅变化时分配新 ID
  并递增 generation，阻止陈旧区域被误用。
- 所有缓存与队列必须有字节上限，所有输入尺寸、候选数量与细化流程必须有保护与预算；超限
  行为必须是显式淘汰或明确错误，不得无界增长或静默丢弃。

## 工程约束

- 使用 C++20 和 CMake（项目最低 3.16，使用 CMakePresets 需 ≥ 3.21，开发与 CI 建议 ≥ 3.25）+
  `CMakePresets.json`；模块目标命名为 `mirador::core`、`mirador::image`、`mirador::cache`、
  `mirador::geometry`、`mirador::fusion`、`mirador::render`，按模块开关。
- 依赖方向指向 Mirador 抽象：可选实现依赖 Core 接口，不得反向依赖；依赖方向由 CMake target
  与架构测试共同锁住。
- 为像素格式、stride、ROI、坐标变换、哈希稳定性、变化 ROI、缓存键与失效、NMS、融合规则、
  稳定 ID 编写测试；坐标测试必须覆盖 0/90/180/270 度方向、非连续 stride、奇数尺寸与往返容差。
  缓存相关测试必须验证模型修订、参数、ROI、预处理版本变化会使旧结果失效，不能只验证命中。
- 属性测试验证任意合法裁剪与缩放变换不产生越界坐标；Backend 使用伪实现注入固定结果，Core
  测试不得下载模型或安装 runtime。
- ASAN/UBSAN 常规运行；涉及跨 session 状态或共享缓存时增加 TSAN/故障注入。
- 变更公开契约时同步更新设计文档、示例与兼容性说明。不得宣称未通过目标平台或基准验证的
  性能或跨平台保证；性能验收必须发布基准环境与数字。

## Git 提交与仓库纪律

Commit、分支、MR、评审与合并必须遵循工程规范第 10 节。要点：

- Commit Message 使用 `<type>(<scope>): <subject>`；type 限于
  `feat`/`fix`/`refactor`/`perf`/`docs`/`test`/`build`/`ci`/`chore`/`revert`，scope 取自
  `core`、`image`、`cache`、`geometry`、`fusion`、`render`、`adapters`、`examples`、
  `benchmarks`、`tests`、`build`、`ci`。一个 Commit 对应一个独立逻辑修改；含义不明确的
  提交说明不可接受。
- `master` 是保护分支，只能经 MR 合入；不得 force-push 或改写已发布历史。
- MR 标题同 Commit 格式，描述必须包含修改内容、修改原因、实际执行的测试和影响范围；
  未执行测试不得填写"测试通过"。
- 提交前检查 `git status` / `git diff` / `git diff --cached`：不包含无关格式化、临时 Debug
  代码、运行日志、编译产物、IDE 文件、大文件（含模型权重与数据集）和敏感信息；不把无关
  工作树改动带入提交。
- `user.name` 与 `user.email` 必须是提交者本人，严禁使用他人身份提交。
- Agent 可以在需要触发或验证 CI 时创建范围化 commit 并以普通非 force 方式 push 当前工作
  分支（含同一请求内修复 CI 失败的后续提交），但不得合并 PR、创建 release/tag、修改仓库
  设置或推送他人分支；这些操作仍需用户明确授权。
- 认证凭据不得打印、复制进仓库、写入远程 URL、暴露于进程参数或日志。`gh` CLI 可用于
  GitHub API 操作，使用前以 `gh auth status` 确认身份。

## 完成定义

一项 Mirador 变更只有在以下条件满足时才算完成：职责位于正确层（Core / 可选模块 / 适配层 /
上层）；核心保持零 runtime、零 OpenCV、零内部线程依赖并被架构测试锁住；取消与超时路径闭合；
失败对调用方可见；缓存受明确字节预算约束且失效规则经过验证；坐标恢复经过方向与往返测试；
相关测试与 sanitizer 通过；计划状态、设计、决策和验收证据已经同步；Commit 与 MR 符合仓库纪律。
