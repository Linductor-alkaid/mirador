# M5：平台适配与产品化基准

> 状态：Completed（工作项与退出条件收口；`v0.2.0` tag 与 PR 合并待负责人授权）
> 负责人：linductor
> 所属计划：[Mirador 实施总计划](mirador-implementation-plan.md)
> 前置：M4
> 发布点：`v0.2.0`（暂定，收尾时经用户授权打 tag/发布）
> 更新日期：2026-09-15

## 目标

落地设计 §24 M5 的产品化收口，四个方向：

1. **POST-05 参考能力后端**：按 `DEC-015` 交付 `integrations/` 参考后端包——
   ncnn 上的 PP-OCR mobile（det+rec）与 YOLO 系检测 Backend，验证真实 runtime 可
   适配性，为评测提供真实能力（`DEC-012` SPI 之外的第一个真实适配面）。
2. **平台适配示例**：在独立适配层接入 Android 截图/Accessibility、Linux 窗口采集
   与 Windows 捕获示例，并落地 M4 留口的 `kDisplay` 坐标空间变换来源（设计 §7、
   §24 M5）。
3. **产品化基准（`SCOPE-08`）**：变化检测、缓存命中路径、Backend 调用外层耗时的
   基准入口与评测集组织，按 `DEC-011` 环境与方法发布数字；GPU 零拷贝路径
   （`POST-01`）是否立项由测量结果决定，不提前扩大 Core。
4. **产品化补齐**：线程安全验证、模糊测试、许可证说明、兼容性记录与 API 文档
   （设计 §22、§23）。

M5 完成后总计划 `SCOPE-07`/`SCOPE-08`/`SCOPE-09`/`SCOPE-10` 具备勾选证据，设计
§26 性能验收按 `DEC-011` 限定口径发布。

## 范围与非目标

范围：`integrations/` 新顶层类别（默认构建零获取）、`MIRADOR_BUILD_INTEGRATIONS`
选项与专用 CI job、ncnn 引入与锁定、OCR/Detector 参考后端（冒烟层 + 真实模型
评测层）、`adapters/capture-linux`（X11，可选目标）与 Windows/Android 采集适配
示例（CI 编译验证）、`benchmarks/` 扩展与 `docs/benchmarks/` 数字发布、评测集
组织、并发/模糊测试入口、`docs/compatibility/`、`THIRD_PARTY_NOTICES` 与供应链
登记、API 文档。
非目标：GPU/native buffer 零拷贝路径（`POST-01`，按测量结果另行立项）；异步
接口（`POST-02`）；光流/Embedder（`POST-03`/`POST-04`）；模型权重与数据集入仓；
VLM/Agent/动作执行；平台权限申请与采集服务化（适配示例只演示调用方注入路径）；
Android 功耗结论（`DEC-011`：挂起至物理设备补跑）。

## 设计与决策依据

- [设计文档](../design/mirador-development-design.md) §7（坐标空间与 `kDisplay`
  变换来源）、§12（缓存）、§20（性能与资源预算、归因口径）、§21（构建与目录、
  integrations 定位）、§22（隐私与许可证）、§23（评测设计）、§24 M5、§26（验收）。
- 已生效：`DEC-001`~`DEC-014`。
- 本里程碑冻结：`DEC-011`（基准环境与方法）、`DEC-015`（POST-05 runtime 选型：
  ncnn 主选、ONNX Runtime 备选、`integrations/` 存放、评测接入分层）。

## 工作项

- [x] `M5-01` 立项：里程碑文档；`DEC-011` 冻结（基准环境与方法口径）；POST-05
  立项（`DEC-015`：runtime 选型、存放形式、评测接入、归因口径）；总计划状态同步
  （M5 启动、`SCOPE-07` 补勾、POST-05 转交付中）。
- [x] `M5-02` `integrations/` 骨架与 ncnn 引入（commit 742a73c，工程规范 §9.2.5
  单独 commit 集）：`MIRADOR_BUILD_INTEGRATIONS`（默认 OFF）、FetchContent pinned
  锁定与 supply-chain 审计登记、`THIRD_PARTY_NOTICES` integrations 分节、架构测试
  确认默认构建图零 runtime 令牌、ncnn 合成 tiny 模型冒烟（forward → Mirador 原始
  结果契约）、专用 CI job。
- [x] `M5-03` OCR 参考后端（PP-OCR mobile，`integrations/ocr_ppocr`）：det 路径
  复用 M3 DB 后处理/轮廓框恢复，rec 路径复用行合并/文本规范化 + CTC 解码；
  权重由使用者显式路径提供；冒烟层无权重可运行。
- [x] `M5-04` Detector 参考后端（YOLO 系，`integrations/detector_yolo`）：
  letterbox/NMS/类别过滤复用 M3 组件；候选数量与缓冲预算显式。
- [x] `M5-05` 平台采集适配示例：`adapters/capture-linux`（X11 窗口采集，可选目标
  默认关闭，Xvfb 下冒烟）；Windows GDI 捕获与 Android MediaProjection +
  Accessibility 适配示例（CI msvc/ndk job 编译验证 + README 边界说明）；`kDisplay`
  变换来源契约落地（外部区域与采集帧的坐标转换由适配层提供 `Transform2D`，
  [DEC-016](../decisions/DEC-016-display-space-transform-contract.md)）。
- [x] `M5-06` 基准扩展与评测集组织（`SCOPE-08`）：缓存命中路径、Backend 外层
  耗时、RSS/体积测量入口；评测集场景清单与离线数据接入约定（静态页、局部动画、
  滚动、弹窗、主题切换、旋转、相似图标，设计 §23）；按 `DEC-011` 发布 Linux x64
  数字到 `docs/benchmarks/`。（发布说明的 `DEC-011` 限定表述随 `M5-09` 发布物
  落地）
- [x] `M5-07` 鲁棒性与并发验证：快照并发读/跨 session 共享缓存测试（TSAN 矩阵
  常规运行）；模糊测试入口（后处理概率图、缓存键序列化、坐标变换输入）随 CI
  可选 job；隐私负向测试复核（默认不落盘、不联网、日志脱敏，`DOD-06`）。
  （跨 session 共享缓存的 TSAN 用例在当前 API 下不可表达——session `create()`
  无缓存注入口、缓存明确非线程安全且归 session 独占；已在
  `tests/fusion/concurrency_test.cpp` 注释记录，矩阵随缓存注入 API 引入扩展）
- [x] `M5-08` 文档收口：公共 API 文档（接口参考 + 示例索引）、`docs/compatibility/`
  登记（OpenCV 等系统包版本区间）、许可证说明完整性复核、README 产品化更新。
  （`SCOPE-10` 的勾选证据随 `M5-09` 终轮 CI 回填）
- [x] `M5-09` 收尾：全 Linux 预设矩阵与 lint、跨平台 CI 证据回填（含
  integrations job）、计划/CHANGELOG 同步、`v0.2.0` 发布准备（用户授权）。
  （`v0.2.0` tag 与 PR 合并待负责人授权；工作项本身收口）

## 风险与阻塞

- `RISK-2026-12`（`DEC-015` 新增）：ncnn 转换链对 PP-OCR/YOLO 特定算子覆盖不确定
  — 处置：备选路径 ONNX Runtime 已冻结，触发即切换，不静默扩写自定义层。
- `RISK-2026-09`（沿用）：一方线段检测器真实场景质量 — 处置：`M5-06` 评测集收口，
  达标与否记录于 `docs/benchmarks/`。
- `RISK-2026-10`/`RISK-2026-11`（沿用）：融合关联与稳定 ID 质量待真实数据对齐 —
  处置：`M5-06` 评测（区域 proposal recall/precision、稳定 ID 延续率）收口；
  `DEC-010` 的触发条件（二分图匹配评估）在此时判定。
- `RISK-2026-01`（沿用）：Android/Windows 采集适配只能 CI 编译验证，运行证据与
  性能补跑受设备限制 — 处置：`DEC-011` 补跑条件。
- 新增 `RISK-2026-13`：真实模型评测依赖权重可得性（不进仓库、不下载），评测数字
  可能延后 — 处置：冒烟层先行闭环；数字按工程规范第 4 节记录补跑条件。

## 测试与退出条件

- [x] 默认构建（全部 6 个 Linux 预设 + 最小核心配置）不获取、不编译、不链接任何
  runtime；架构测试覆盖 `integrations/` 令牌规则（`src/`/`include/` 不出现 ncnn，
  integrations 不被核心目标引用）。（终轮 head：6 预设全绿——debug 43/43，其余
  42/42；最小核心配置仅产出 libmirador_core.a；CI 13/13 绿含架构套件）
- [x] `MIRADOR_BUILD_INTEGRATIONS=ON` 专用 CI job 绿：pinned ncnn 拉取、构建、
  合成模型冒烟通过；锁定信息与 supply-chain/许可证文档同步。（CI 终轮绿；
  `integrations/deps.lock.json` 与 `THIRD_PARTY_NOTICES` 第 3 节一致）
- [x] OCR/Detector 参考后端冒烟层通过（无权重可运行）；真实模型评测按 `DEC-011`
  记录或明确记录补跑条件（权重与设备）。（冒烟随 CI integrations job；补跑条件
  记录于 `RISK-2026-13` 与 M5-03/M5-04 验证记录）
- [x] 采集适配：Linux X11 适配在 Xvfb 冒烟通过；Windows/Android 适配在 CI 编译
  验证；`kDisplay` 变换有方向与往返容差测试（`DOD-03`）。（PR #12 CI 12/12 绿；
  本机 XWayland 分支冒烟 + CI xvfb 分支冒烟通过）
- [x] 基准：`benchmarks/` 覆盖变化检测、缓存命中路径、Backend 外层耗时、RSS/体积；
  `docs/benchmarks/` 发布 Linux x64 数字（含环境四元组与复现命令）；发布说明
  携带 `DEC-011` 限定表述。（CHANGELOG 0.2.0 兼容性影响节含限定表述）
- 并发/模糊/隐私：TSAN 矩阵含快照并发读与跨 session 缓存用例；模糊入口可运行；
  `DOD-06` 负向测试复核通过。（共享缓存用例的 API 缺口见工作项注记）
- [x] 文档：API 文档、`docs/compatibility/`、`THIRD_PARTY_NOTICES`、supply-chain
  登记完整；`SCOPE-07`~`SCOPE-10` 具备勾选证据。（终轮 CI 13/13 绿即 `SCOPE-10`
  证据）
- [x] `DEC-011`/`DEC-015` 状态 Accepted 并被工作项引用；总计划与 CHANGELOG 同步。
  （`DEC-016` 同为 Accepted；CHANGELOG 0.2.0 条目就绪）

## 验证记录

2026-09-15：里程碑创建（`M5-01`）。依据设计文档 §7/§20/§21/§22/§23/§24 M5/§26 与
总计划 `SCOPE-07`~`SCOPE-10`、`POST-05` 拆分工作项 `M5-01`~`M5-09`；冻结 `DEC-011`
（基准环境与方法：Linux x64 主环境、CI runner 不作性能证据、Android 模拟器仅功能
验证、物理 Android/Windows 记录补跑条件）与 `DEC-015`（ncnn 主选 + ONNX Runtime
备选、`integrations/` 默认零获取、评测接入分层、归因口径）。发布点 `v0.2.0` 暂定，
待 `M5-09` 后经用户授权打 tag/发布。

2026-09-15：`M5-02` 实施完成并经 Independent-Verification-Agent 两轮验证通过
（分支 `feat/m5-platform-adapters-and-benchmarks`，commit 742a73c）。

- 环境：Ubuntu 24.04 x64（GCC 13.3.0、CMake 3.28.3 + Ninja、clang-format/
  clang-tidy 18.1.3）；ncnn 20260526 经 FetchContent 拉取（校验
  `integrations/deps.lock.json` 与 CMake pin 一致）。
- 落地内容：`integrations/` 顶层类别与 `MIRADOR_BUILD_INTEGRATIONS`（默认 OFF）；
  `NcnnRuntime` PIMPL 包装（公开 `blobs()` 查询、planar CHW 张量、打包前/后取消
  轮询、`kBackendUnavailable/kBackendFailure/kInvalidArgument` 显式传播）；
  `pack_image`（Gray8/Rgb8/Rgba8 → CHW，尊重 stride）；合成 tiny 模型冒烟（28 项
  断言：数值对照独立朴素卷积、确定性、NV12 拒绝、缺失权重、取消、未知 blob、
  stride 行采样）；架构扫描新增 runtime 令牌规则（仅限 `integrations/`，负向注入
  验证生效）；CI 新增 `integrations-ncnn` job；supply-chain 登记（ncnn.md、
  dependency-policy、`THIRD_PARTY_NOTICES` 第 3 节）。
- 验证（独立验证代理编写并执行测试）：首轮报告 4 类实现缺陷（protected
  `find_blob_index_by_name` 误用、`pack_image` HWC 违反 CHW 契约、
  `enable_testing()` 时序致 `add_test` 失效、format/tidy 违规），主循环修复
  （commit 742a73c）后复验：integrations 套件 ctest **40/40**、debug 预设回归
  **40/40**、clang-format 与 clang-tidy 双口径（退出码 + error 行数）归零、临时
  目录无残留。缺陷发现与修复过程完整记录，无吞掉的失败。
- 限制：CI `integrations-ncnn` job 首轮因 runner CMake 不默认导出编译数据库而
  tidy 失败（本地 3.28 默认导出掩盖了该差异），修复为显式
  `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`（commit c78b878），证据随本轮 CI 回填。

2026-09-15：`M5-06` 基准入口扩展实施完成并经独立验证通过（commit bad4822）。

- 落地内容：`benchmarks/cache_backend_bench.cpp`（`mirador_bench_cache_backend`，
  链接 `mirador::fusion`）——能力缓存 raw lookup hit/miss p50/p95、
  `PerceptionSession::run_ocr` 缓存命中外层开销（不变量断言：同帧内容不重复调用
  Backend）、`kRefresh` 完整 miss 路径外层开销、峰值 RSS（VmHWM）。
- 口径说明：miss 场景用 `cache_policy=kRefresh` 绕过缓存读制造确定性 100% miss，
  不依赖场景 dHash 量化特性；该场景为「无淘汰稳态下的 miss 外层开销」（同键
  replace），不表述为冷缓存填充动态（验证代理记录性意见，写入后续
  `docs/benchmarks/` 报告口径）。
- 验证（独立验证代理执行）：debug/asan 运行退出码 0 且不变量成立、无 sanitizer
  报告；release 主环境数字（Ubuntu 24.04 x64 本机，按 `DEC-011` 仅对本机构建
  有效）：raw cache hit p50 0.215 µs / miss 0.078 µs；`run_ocr` hit p50 2196 µs /
  miss 2202 µs（外层成本由全帧指纹计算主导，命中收益 = 免 Backend 调用）；
  VmHWM 14.46 MiB；复跑波动 < 1%。`ctest --preset debug` 40/40 回归通过；
  format/tidy 双口径归零。
- 限制：`docs/benchmarks/` 正式报告与体积测量随 `M5-06` 收口（本条为入口与数字
  初稿）；Android/Windows 数字按 `DEC-011` 待补跑。

2026-09-15：`M5-05` Linux X11 采集适配器实现完成（commit 7b67557），
`adapters/capture-linux`（`MIRADOR_BUILD_ADAPTERS_CAPTURE_LINUX` 默认 OFF）：
`X11Capture` RAII 连接管理、BGRX→RGB8 转换（Frame 持有缓冲）、打包循环取消轮询、
Xlib `Status` 宏污染 `#undef` 隔离、XDestroyImage 宏经结构体函数指针等价替代。
lint 双口径归零。合成窗口冒烟测试与 CI `capture-adapter` job（xvfb-run）验证
随 `M5-05` 收口进行。

2026-09-15：`M5-03` PP-OCR 参考后端实施完成并经 Independent-Verification-Agent
验证通过（commit 66fd74e）。

- 落地内容：`integrations/ocr_ppocr/`——`ctc_decode_greedy`（贪心 CTC：折叠、
  blank 分隔、softmax 置信度、平局取首类、参数校验）；`PpOcrDetBackend`
  （letterbox 复用 → ncnn forward → 概率图 → M3 `db_postprocess_aabb` → 逆
  变换恢复 prepared 空间 + 裁剪）；`PpOcrRecBackend`（整图单行假设、字典文件
  调用方提供、CHW c=T/w=C 输出契约冻结）；`PpOcrBackend` 组合管线（det → 行
  排序 → 逐框 crop → rec → 置信度乘积）。`NcnnRuntime` 增补默认构造（moved-from
  语义）。全部复用 M3 组件与 `mirador::core` 契约，无核心改动。
- 验证（独立验证代理编写并执行）：合成模型冒烟 40 项断言全过（det 数值/边界/
  置信度、rec "ABCD" 解码、组合管线、CTC 平局与非法参数、工厂负路径）；冒烟
  模型运行时生成、权重不入库、临时目录清理。integrations 套件 **41/41**、
  debug 预设回归 **40/40**、5 个源文件 tidy 双口径归零、format 归零、asan 下
  两个 smoke 无报告。规格侧两处偏差按 pinned ncnn 适配并记录：Convolution 参数
  键位（8=int8_scale_term、11/12/13/14-16=h/dilation/stride/pad，与本决策草稿
  不同）、CTC 标准语义（[1,1,2,0,2]→[1,2,2]，blank 为分隔符）。
- 限制：真实 PP-OCR 权重的评测按 `DEC-015` 分层由负责人提供权重后运行
  （`RISK-2026-13`）；asan 预设与 integrations 组合未入预设文件，如需 CI 覆盖
  再立预设。

2026-09-15：`M5-05` Linux X11 采集适配器冒烟验证通过（commit d4ac878，验证由
Independent-Verification-Agent 执行）。

- 冒烟测试（16 项断言全过）：测试仅创建并捕获自有的 64x48 X 窗口（不读屏幕
  其他内容、不落盘、不打印像素，`RULE-10`）；覆盖 create/root 查询/窗口捕获
  像素与尺寸不变量（±8 容差）/几何查询/取消路径/坏 display/未知窗口
  （kBackendFailure）/资源清理；安装非致命 X 错误处理器防止 Xlib 默认 handler
  的 exit 掩盖负路径（该语义已写入 `x11_capture.hpp` 契约注释）。
- **环境发现**：本机 `:0` 为 XWayland（`XDG_SESSION_TYPE=wayland`）——自有
  窗口捕获正常，root 整屏捕获因 XWayland 无 root 像素后备而 `kBackendFailure`
  （属文档化行为，非适配器缺陷）。测试对 root 场景按环境分支断言（XWayland →
  失败语义；Xorg/Xvfb → 完整不变量），真实 X 分支由 CI `capture-adapter` job
  （xvfb）覆盖；真实 Xorg 桌面下的整屏采集适用性待物理环境补验。
- 验证：capture 构建 + smoke ctest 1/1、debug 预设回归 40/40、tidy/format 双
  口径归零、asan 等价构建下 smoke 无报告。

2026-09-15：`M5-04` YOLO 系参考检测后端实施完成并经 Independent-Verification-Agent
验证通过（commit 9d0c69c）。

- 落地内容：`integrations/detector_yolo/`——`YoloDetectorBackend`
  （`mirador::DetectorBackend` SPI）：冻结 YOLOv5 单张量输出契约（CHW
  channels=1/height=提案数/width=5+C，行 = [cx,cy,w,h,obj,class…], v8 分头等
  异构输出在 param 内归一）；letterbox 复用 + x/255 归一化；objectness 乘法
  可配置；逆变换恢复 prepared 空间 + 裁剪；M3 `nms` 复用（class-aware 可配）；
  `max_candidates` 显式预算（超出即 `kBudgetExceeded`，RULE-06）；类别名表可
  选。解码循环独立成函数（复杂度阈值内）。
- 验证（独立验证代理编写并执行）：合成模型冒烟 28 项断言全过——双提案解码与
  逆变换精确映射（128×128 → letterbox 0.5 → ×2）、重复框 NMS 抑制、
  min_confidence 过滤、objectness 开/关两口径、显式预算报错、取消、工厂负
  路径、info 往返。integrations 套件 **42/42**、debug 回归 **40/40**、lint 双
  口径归零、asan 等价构建无报告。
- 限制：真实 YOLO 权重的评测按 `DEC-015` 分层由负责人提供权重后运行
  （`RISK-2026-13`）；v8 分头模型的 param 归一层未在真实模型上验证（冒烟覆盖
  契约本身）。

2026-09-15：`M5-05` kDisplay 契约与 Windows/Android 采集适配实施完成
（分支 `feat/m5-display-contract-and-capture-adapters`；全部测试由
Independent-Verification-Agent 编写并执行）：

- `DEC-016` 冻结（Accepted）：kDisplay 变换来源为适配层/调用方提供的
  `Transform2D`（kOriented→kDisplay）；`FusionOptions::display_transform` 必备
  语义（kDisplay 参与即必须存在）、`EvidenceSet` 三空间接受、转换链与
  `run_*` 输出空间保持帧族的边界收窄。
- 核心落地：`src/fusion/fusion.cpp` 转换链（kDisplay↔kOriented↔kFrame 四条
  新路径）与选项校验、`src/fusion/evidence.cpp` 空间白名单、设计 §16 M5 冻结
  补充同步。
- 独立验证（两轮）：首轮报告 2 类实现缺陷（`display_transform` optional 的
  无条件解引用、空证据集绕过 transform 必备检查），主循环修复后复验通过；
  debug 套件 **41/41**（新增 `display_space_test`：9 组合方向矩阵、k0/90/180/
  270 往返容差 1e-6、session kDisplay 快照端到端、7 类负路径）、capture 套件
  **42/42**（X11 冒烟新增 display_transform 平移/恒等/未知窗口断言 10 项）、
  asan/ubsan 无报告、lint 双口径归零。
- Windows 适配示例：`adapters/capture-windows`（GDI BitBlt + DIB section，
  `MIRADOR_BUILD_ADAPTERS_CAPTURE_WINDOWS` 默认 OFF，仅 WIN32）；CI windows
  job 编译验证（本机无 Windows 运行环境，运行冒烟按 `DEC-011` 记录补跑条件）。
- Android 适配示例：`adapters/capture-android`（纯 C++ Accessibility 转换 +
  投影 display transform 有宿主测试 15 项断言全过；AImageReader 投影采集与
  JNI 桥仅 NDK 构建）；CI android job 编译验证（真机运行按 `DEC-011` 补跑）。
- CI：`capture-adapter` job 扩展为 `capture-adapters`（+Android 宿主测试 +
  host 可编译适配源 tidy）；lint 排除表同步。
- CI 证据（PR #12，两轮）：首轮 9/12 绿，3 类失败——clang-format（未跟踪目录
  文件漏出本地检查口径）、capture-adapters 的 IWYU 直接包含、NDK 下
  `AImage_getPlaneData` 的 `uint8_t**` 签名与头文件 default 析构冲突；修复
  commit d55c552 后第二轮 **12/12 全绿**（含 msvc 编译 GDI 适配、ndk 编译
  MediaProjection/JNI、capture-adapters job 42/42 + xvfb X11 冒烟）。

2026-09-15：`M5-06` 收口（PR #12 分支续交付）：

- 新增 `benchmarks/measure_sizes.sh`（模块静态库 + 可执行文件体积表，缺省
  构件按模块开关跳过）；变化检测基准数字与体积表发布于
  [linux-x64-change-detection-sizes-2026-09](../benchmarks/linux-x64-change-detection-sizes-2026-09.md)
  （release 构建，同机口径）：unchanged p50 1.64 ms / 全量路径 ≈ 10.8 ms，
  六模块静态库合计 0.61 MiB（core+image+cache ≈ 326 KiB）。
- 评测集场景清单与离线数据接入约定发布于
  [evaluation-scenes](../benchmarks/evaluation-scenes.md)（场景 ID、
  覆盖级别、manifest 字段、隐私与确定性约定，数据不入仓）。
- 原 cache-backend 报告的"体积待 M5-09"限制更新为已登记；`SCOPE-08` 具备
  勾选证据。纯文档与脚本变更，无公共 API 影响；脚本在 release 构建上实际
  执行验证。

2026-09-15：`M5-07` 实施完成（PR #12 分支续交付；全部测试由
Independent-Verification-Agent 编写并执行，两轮）：

- 并发矩阵：`tests/fusion/concurrency_test.cpp`——已发布快照跨代并发读
  （4 读者 × 主线程连续 fuse，读者持旧快照存活）与会话级并行（双 session
  双线程完整管线）；TSAN 预设 42/42 零报告，并发二进制复跑 5 次干净。
  跨 session 共享缓存用例在当前 API 下不可表达（无缓存注入口），注释记录
  于测试头部。
- 模糊入口：`tests/fuzz/`（DB 概率图后处理、缓存键序列化、坐标变换，
  `MIRADOR_BUILD_FUZZ` 默认 OFF、clang-only、address+undefined+fuzzer 且
  `-fno-sanitize-recover`）；CI 新增 `clang / fuzz` job（每 harness 限时
  30 s）。本地等效验证：db/cache 各 ≥2 万 runs、transform 约 112 万 runs
  零 finding；fuzzer 抓到的均为 harness 自身缺陷（有符号溢出、NaN 比较、
  传递包含），已修复。
- 产品健壮性修复（主循环）：`mirador::inverse` 奇异性检查扩展到非有限
  行列式，并对逆矩阵元素做有限性校验——修复前 det=+inf 返回 ok + 全零
  逆矩阵（往返 NaN），fuzz 发现、回归测试（`Transform.InverseRejects*`）
  与 fuzz 不变量（ok ⇒ 全元素有限）双向锁定。
- 隐私负向（`DOD-06`）：`tests/privacy/privacy_test.cpp`——完整管线
  （含显式缓存写入）前后 cwd/temp 目录零新增文件；双标记证据流经成功与
  5 类失败路径，全部 `Status` message 与 `FusionTrace` 字符串字段零泄漏
  （`static_assert` 钉死 trace 成员为数值类型）；src/ 无任何日志输出代码
  （grep 佐证）。
- 验证：debug 43/43、tsan 42/42（setarch -R）、asan/ubsan 全绿、触碰文件
  format/tidy 双口径归零。本机无 clang，验证代理从 Ubuntu 源提取
  clang-18 到用户目录以 CI 同口径执行 fuzz 构建与 tidy。CI fuzz job 证据
  随本分支 push 回填。

2026-09-15：`M5-08` 文档收口（PR #12 分支续交付；纯文档变更）：

- [docs/api/README.md](../api/README.md)：模块 → 公共头 → 关键类型导航索引，
  适配层/integrations 开关表，五个使用示例索引；契约正文保持在头文件注释，
  索引不复制契约。
- [docs/compatibility/compatibility.md](../compatibility/compatibility.md)：
  编译器/CMake/NDK/依赖的已验证版本表、平台功能可用性矩阵（✅/🔨/⏳/❌
  四级口径）、已知行为差异（TSAN/ASLR、XWayland root、JNI modified UTF-8）。
- `THIRD_PARTY_NOTICES` 新增第 4 节：平台采集库（libX11/Win32 GDI/Android
  NDK runtime）integrator-provided、不随库分发；第 1-3 节复核无过期项。
- README 产品化：M5 当前状态、目录结构（adapters/integrations/docs）、可选
  构建面六开关、基准入口补全。
- 许可证复核结论：仓库内第三方仍仅 googletest（pinned）；OpenCV/ncnn/平台库
  均为 integrator-provided 且登记完整；ELSED 仍未引入（引入窗口见 `DEC-009`）。

2026-09-15：`M5-09` 收尾完成（PR #12 分支；tag 待授权）：

- 终轮 head 全 Linux 预设矩阵：debug 43/43、release/warnings/asan/ubsan/tsan
  各 42/42（tsan 经 `setarch -R`）；clang-format 与 clang-tidy 双口径归零；
  最小核心配置（仅 `mirador::core`）独立构建通过。
- 跨平台 CI 证据：终轮 head（63ba70f 后续 docs commit 触发轮）CI 13/13 全绿——
  linux 6 预设矩阵、msvc（含 GDI 适配编译）、ndk（含 Android 适配编译）、
  opencv 适配、integrations-ncnn、capture-adapters、新增 fuzz job、lint。
- CHANGELOG `0.2.0` 条目就绪（含 `DEC-011` 限定表述与兼容性影响节）；
  总计划 `SCOPE-10` 勾选、M5 状态 Completed。
- 待负责人授权事项：PR #12 合并、`v0.2.0` tag/发布。
