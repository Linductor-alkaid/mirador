# M5：平台适配与产品化基准

> 状态：In Progress
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
- [ ] `M5-03` OCR 参考后端（PP-OCR mobile，`integrations/ocr_ppocr`）：det 路径
  复用 M3 DB 后处理/轮廓框恢复，rec 路径复用行合并/文本规范化 + CTC 解码；
  权重由使用者显式路径提供；冒烟层无权重可运行。
- [ ] `M5-04` Detector 参考后端（YOLO 系，`integrations/detector_yolo`）：
  letterbox/NMS/类别过滤复用 M3 组件；候选数量与缓冲预算显式。
- [ ] `M5-05` 平台采集适配示例：`adapters/capture-linux`（X11 窗口采集，可选目标
  默认关闭，Xvfb 下冒烟）；Windows GDI 捕获与 Android MediaProjection +
  Accessibility 适配示例（CI msvc/ndk job 编译验证 + README 边界说明）；`kDisplay`
  变换来源契约落地（外部区域与采集帧的坐标转换由适配层提供 `Transform2D`）。
- [ ] `M5-06` 基准扩展与评测集组织（`SCOPE-08`）：缓存命中路径、Backend 外层
  耗时、RSS/体积测量入口；评测集场景清单与离线数据接入约定（静态页、局部动画、
  滚动、弹窗、主题切换、旋转、相似图标，设计 §23）；按 `DEC-011` 发布 Linux x64
  数字到 `docs/benchmarks/`。
- [ ] `M5-07` 鲁棒性与并发验证：快照并发读/跨 session 共享缓存测试（TSAN 矩阵
  常规运行）；模糊测试入口（后处理概率图、缓存键序列化、坐标变换输入）随 CI
  可选 job；隐私负向测试复核（默认不落盘、不联网、日志脱敏，`DOD-06`）。
- [ ] `M5-08` 文档收口：公共 API 文档（接口参考 + 示例索引）、`docs/compatibility/`
  登记（OpenCV 等系统包版本区间）、许可证说明完整性复核、README 产品化更新。
- [ ] `M5-09` 收尾：全 Linux 预设矩阵与 lint、跨平台 CI 证据回填（含
  integrations job）、计划/CHANGELOG 同步、`v0.2.0` 发布准备（用户授权）。

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

- [ ] 默认构建（全部 6 个 Linux 预设 + 最小核心配置）不获取、不编译、不链接任何
  runtime；架构测试覆盖 `integrations/` 令牌规则（`src/`/`include/` 不出现 ncnn，
  integrations 不被核心目标引用）。
- [ ] `MIRADOR_BUILD_INTEGRATIONS=ON` 专用 CI job 绿：pinned ncnn 拉取、构建、
  合成模型冒烟通过；锁定信息与 supply-chain/许可证文档同步。
- [ ] OCR/Detector 参考后端冒烟层通过（无权重可运行）；真实模型评测按 `DEC-011`
  记录或明确记录补跑条件（权重与设备）。
- [ ] 采集适配：Linux X11 适配在 Xvfb 冒烟通过；Windows/Android 适配在 CI 编译
  验证；`kDisplay` 变换有方向与往返容差测试（`DOD-03`）。
- [ ] 基准：`benchmarks/` 覆盖变化检测、缓存命中路径、Backend 外层耗时、RSS/体积；
  `docs/benchmarks/` 发布 Linux x64 数字（含环境四元组与复现命令）；发布说明
  携带 `DEC-011` 限定表述。
- [ ] 并发/模糊/隐私：TSAN 矩阵含快照并发读与跨 session 缓存用例；模糊入口可运行；
  `DOD-06` 负向测试复核通过。
- [ ] 文档：API 文档、`docs/compatibility/`、`THIRD_PARTY_NOTICES`、supply-chain
  登记完整；`SCOPE-07`~`SCOPE-10` 具备勾选证据。
- [ ] `DEC-011`/`DEC-015` 状态 Accepted 并被工作项引用；总计划与 CHANGELOG 同步。

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
