# M2：Backend SPI 与能力结果缓存

> 状态：Complete
> 负责人：linductor
> 所属计划：[Mirador 实施总计划](mirador-implementation-plan.md)
> 前置：M1
> 建议发布点：`v0.1.0-beta.2`
> 更新日期：2026-09-14

## 目标

冻结 Backend SPI 公共契约（`BackendInfo` 能力与实现身份、`OcrBackend`/`DetectorBackend`
接口、请求与原始结果类型、`ExecutionContext` 取消通道），落地能力结果缓存（`RULE-07`
缓存键、显式刷新与失效规则、字节预算），并引入 `PerceptionSession` 的首个可执行闭环：
提交帧 → 变化检测与缓存判断 → 按需 Backend 能力执行 → 坐标恢复与结果复用。全部测试以
Fake Backend 注入固定结果完成，不依赖任何模型 runtime（设计文档 §9、§12、§18、§24 M2）。

## 范围与非目标

范围：core 新增 SPI 契约类型；`mirador::cache` 新增能力结果缓存与键构造；`mirador::image`
新增有界 `ChangeSignature`（会话跨帧状态不保留整帧）；`mirador::fusion` 转编译目标并承载
`PerceptionSession`（fusion 是设计 §5 中 image 与 cache 的汇合点）；架构测试同步演进；
Fake Backend 端到端测试；无 runtime 示例更新。
非目标：真实模型 runtime 适配（`POST-05`，触发条件未满足）、语义快照缓存与视觉索引
（M3）、证据融合/稳定 ID/`SemanticSnapshot`（M4）、NMS/letterbox 组合器与逐字符 OCR
结果（随首个需要的 Backend 引入，见 `DEC-012`）、缓存命中路径基准（M5，`SCOPE-08`）。

## 设计与决策依据

- [设计文档](../design/mirador-development-design.md) §9（Backend SPI）、§12（能力结果
  缓存）、§13/§14（OCR 与检测请求字段）、§18（会话与 `ExecutionContext`）、§19（错误）、
  §24 M2。
- 已生效：`DEC-001`（同步 API）、`DEC-002`（Core 不链接模型 runtime）、`DEC-003`（公共
  API 不暴露 OpenCV 类型）、`DEC-004`（`Result<T>`/`Status`）、`DEC-005`（构建基线）、
  `DEC-006`（GoogleTest）、`DEC-007`（多平面表示）。
- 本里程碑冻结：`DEC-008`（缓存默认字节预算：帧 4 MiB、能力结果 16 MiB）、`DEC-012`
  （Backend SPI 契约：请求分层、坐标契约、`ExecutionContext` 参数、线程安全声明、
  Embedder 延后）、`DEC-013`（`PerceptionSession` 归属 fusion 与模块依赖演进）。

## 工作项

- [x] `M2-01` core Backend SPI 契约：`ExecutionContext`（`is_cancelled` 回调 + 可选
  deadline 及判定辅助）、`BackendInfo`（含 `thread_safe` 显式声明与 `validate`）、
  `TextRegion`/`DetectionRegion` 原始结果、`OcrRequest`/`DetectionRequest`（ROI 与空间、
  `min_confidence`、`max_side`、`backend_params`、`output_space`、`CachePolicy`）、
  `OcrBackend`/`DetectorBackend` 抽象接口（`info()` + 携带 `ExecutionContext` 的执行方法）；
  设计文档 §9/§13/§18 同步。
- [x] `M2-02` cache `CapabilityResultCache`：`RULE-07` 全字段的缓存键构造与稳定 128 位
  摘要（跨平台确定，非 `std::hash`）；字节预算 LRU（淘汰顺序、替换失效、单条目超预算
  显式 `kBudgetExceeded` 且状态不变）；请求参数摘要函数；键字段敏感性负向测试（指纹、
  ROI、预处理版本、Backend 身份、模型修订、参数、输出空间逐项变化必失配，`DOD-04`）；
  `DEC-008` 冻结。
- [x] `M2-03` image 有界 `ChangeSignature`（帧尺寸 + 指纹 + 灰度缩略图）与签名版
  `detect_change` 重载：会话只保留上一帧紧凑签名、不保留整帧；与视图版
  `detect_change` 输出位一致；缩略图尺寸不匹配显式报错。
- [x] `M2-04` fusion 转编译目标与 `PerceptionSession`：`analyze_change`（首帧语义）、
  `run_ocr`/`run_detector`（ROI 裁剪 → 格式转换 → `max_side` 缩放的确定预处理链、
  Backend 格式门控、坐标按 `output_space` 恢复、缓存读/写/刷新策略、取消与 deadline 检查）；
  架构测试演进为 fusion 链接接口恰为 core+image+cache 并新增链接闭包探针。
- [x] `M2-05` Fake Backend 端到端测试：命中/未命中/`kRefresh`/`kReadOnly`、模型修订与
  参数变化失效、坐标恢复矩阵（0/90/180/270、非连续 stride、奇数尺寸、ROI+缩放往返）、
  取消/超时、Backend 缺失与信息非法、格式不可达、源不匹配、预算超限。
- [x] `M2-06` 无 runtime 示例与文档同步：示例演示"提交帧 → 变化检测 → Fake/桩 Backend
  执行 → 缓存命中"，公共 API 纳入编译验证；设计文档、README、CHANGELOG、`DEC-008`/
  `DEC-012`/`DEC-013` 同步。
- [x] `M2-07` 里程碑收尾：全 Linux 预设矩阵与 lint 通过、跨平台 CI 证据回填、验证记录
  与计划状态更新。

## 风险与阻塞

- `RISK-2026-01`：MSVC/NDK 跨平台编译证据依赖 CI，受限时按规范记录补跑条件。
- `RISK-2026-04`：缓存键设计遗漏导致跨模型/参数误命中 — 处置：`M2-02` 键字段逐项敏感
  性负向测试与 `DOD-04` 失效矩阵。
- `RISK-2026-07`（新增）：SPI 契约若允许 Backend 输出坐标空间含糊，坐标恢复会退化为
  Backend 私有细节（违背 `RULE-05`）— 处置：`DEC-012` 冻结"Backend 输出落在
  prepared 图像像素空间、恢复由 Mirador 完成"的坐标契约并以端到端矩阵锁定。
- `POST-05`（ncnn/ONNX Runtime 示例适配包）触发条件未满足：Fake Backend 已能验证 SPI
  可适配性，本机与 CI 均无 runtime 环境；维持延后，触发时按工程规范 §9 立项。

## 测试与退出条件

- [x] 全部 6 个 Linux 预设（debug/release/warnings/asan/ubsan/tsan）配置、构建、ctest
  通过；触及文件 `clang-format`/`clang-tidy` 无告警。
- [x] 缓存键：`RULE-07` 每个字段单独变化产生不同摘要；相同字段跨进程内稳定一致。
- [x] 缓存预算与失效：LRU 淘汰顺序、替换旧值失效、单条目超预算显式错误且缓存不变；
  `kRefresh` 绕过读取、`kReadOnly` 不写入（负向路径，不只验证命中，`DOD-04`）。
- [x] 会话闭环：Fake Backend 下提交帧 → `analyze_change` → `run_ocr`/`run_detector`
  全链路通过；第二次相同请求命中缓存（Fake 调用计数不变）。
- [x] 坐标恢复：0/90/180/270 方向、非连续 stride、奇数尺寸、ROI 裁剪与 `max_side`
  缩放组合下，恢复坐标与解析期望在容差内一致（`DOD-03` 矩阵）。
- [x] 取消与超时：`is_cancelled` 与过期 deadline 分别显式报 `kCancelled`/`kTimeout`，
  不静默吞掉；Backend 缺失/信息非法报 `kBackendUnavailable`，格式不可达报
  `kUnsupportedFormat`。
- [x] 架构测试演进后：fusion 链接闭包仅 core+image+cache + 标准库；公共头与 `src/`
  无第三方与线程令牌；CI 全部 job 运行。
- [x] `DEC-008`/`DEC-012`/`DEC-013` 冻结为 Accepted；设计文档 §9/§13/§18 与示例同步。

## 验证记录

2026-09-14：里程碑创建。依据设计文档 §9/§12/§18/§24 M2 与总计划 `SCOPE-01`/`SCOPE-03`
拆分工作项 `M2-01`~`M2-07`；`DEC-008` 按暂定值（帧 4 MiB、能力结果 16 MiB）进入实现并
在本里程碑冻结；Embedder SPI 按 `POST-04` 触发条件延后（`DEC-012` 记录 SPI 的可扩展
约束）。

2026-09-14：`M2-01`~`M2-06` 实施完成（分支 `feat/m2-backend-spi-and-session`）。

- 环境：Ubuntu 24.04 x64（GCC 13.3.0、CMake 3.28.3 + Ninja、clang-format/clang-tidy
  18.1.3）。
- 落地内容：
  - `M2-01`：core 新增 `execution_context`/`backend_info`/`ocr_backend`/`detector_backend`
    /`cache_policy` 公共契约及判定辅助与 `validate(BackendInfo)`；接口可被具体类实现并
    经基类指针调用（单测覆盖）。
  - `M2-02`：`CapabilityResultCache` 与 `capability_cache_digest`（双 FNV-1a 域分隔
    128 位摘要，逐字段喂入无分配）、OCR/检测请求参数摘要；键字段敏感性矩阵（11 个键
    字段逐一失配 + 参数摘要对管线字段不敏感的正向断言）。
  - `M2-03`：`make_change_signature` + 签名版 `detect_change`；视图版 `detect_change`
    保留"指纹早退跳过缩略图重采样"的成本语义，两条路径报告位一致（parity 测试锁定）。
  - `M2-04`：fusion 转编译目标（架构测试演进为按模块允许接口表，新增
    `link_closure_fusion` 探针）；`PerceptionSession::analyze_change` 首帧
    `kGlobal + kFirstFrame`；`run_ocr`/`run_detector` 统一引擎（traits 参数化）：
    预检 → ROI 解析 → 指纹与键构造 → 缓存读 → 预处理链（crop→convert→resize，
    `kPreprocessPipelineVersion`）→ Backend 执行 → 坐标恢复（kFrame/kOriented/kCropped）
    → 缓存写。
  - `M2-05`：`tests/fusion/` 25 项端到端用例（缓存命中/刷新/只读/模型修订与参数失效、
    旋转 90/180/270 恢复往返、padding stride、奇数尺寸与 ROI 取整、`max_side` 回映射、
    kFrame ROI 空间映射、格式转换与不可达格式、null/非法 Backend、Backend 失败传播、
    取消/超时零调用、源不匹配、非法请求、预算超限显式报错、A→B→A 内容回归缓存命中）。
  - `M2-06`：示例 `perception_session_tour`（Stub Backend 演示闭环与缓存复用）；
    设计文档 §9/§13/§18、`DEC-008`/`DEC-012`/`DEC-013`、README、CHANGELOG 同步。
- 本地命令与结果：
  - 全部 6 个 Linux 预设（debug/release/warnings/asan/ubsan/tsan）configure + build +
    ctest 21/21 通过（tsan 经 `setarch "$(uname -m)" -R`，ASLR 内存映射问题与 M1 相同）。
  - OpenCV 适配器 ON：ctest 22/22；`MIRADOR_BUILD_FUSION=OFF`：构建绿且 ctest 19/19
    （fusion 目标与用例正确消失）；image/cache/fusion 全 OFF：最小核心构建绿。
  - touched 文件 `clang-format --dry-run --Werror` 与
    `clang-tidy --warnings-as-errors='*'` 无告警；示例运行输出符合设计（首帧
    kGlobal/kFirstFrame、缓存 1 条目 169 字节、kRefresh 覆盖）。
- 限制：跨平台编译证据待 CI 运行回填（`M2-07`）；变化检测基准入口在 release 构建中
  可运行，本里程碑未新增性能声明。

2026-09-14：`M2-01`~`M2-07` 跨平台编译证据回填，里程碑状态置为 Complete。GitHub
Actions run `34810114741`（[PR #6](https://github.com/Linductor-alkaid/mirador/pull/6)）
10/10 job success：linux gcc/clang debug、gcc warnings/asan/ubsan/tsan、gcc
opencv-adapter、windows msvc/ninja、android ndk arm64-v8a、clang-format/clang-tidy lint。
SPI/cache/session 公共头在 GCC、Clang、MSVC 下编译通过，NDK arm64-v8a 交叉编译通过；
架构测试（source_scan、link_closure、link_closure_image、link_closure_cache、
link_closure_fusion）在全部 job 通过。

2026-09-14：PR #6 经用户授权合并（merge commit），发布点 `v0.1.0-beta.2` 发布：tag 打在
PR #6 合并提交，GitHub Pre-release，发布说明见 CHANGELOG 对应版本段。`v0.1.0-beta.1`
同批补齐发布（tag 打在 PR #5 合并提交，即 M1 收尾点）。M2 全部工作项、退出条件与
发布点均已闭合。
