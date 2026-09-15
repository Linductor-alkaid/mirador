# DEC-015：POST-05 参考能力后端的 runtime 选型、存放形式与评测接入

> 状态：Accepted（2026-09-15，M5 立项窗口内冻结）
> 日期：2026-09-15
> 负责人：linductor
> 冻结里程碑：M5（POST-05 立项，早于 M5 评测准备启动）
> 替代/被替代：无

## 背景与问题

总计划 1.1 修订将 `POST-05`（参考能力后端交付包：OCR 如 PP-OCR mobile、检测如 YOLO 系）
由延后项升级为计划性独立交付物，并要求立项时按工程规范 §9.3 完成 runtime 选型对比、
许可证审查并形成决策记录，同时确认存放形式与评测接入方式。立项必须回答四个问题：

1. 参考 Backend 采用哪个模型 runtime（总计划点名 ncnn / ONNX Runtime 等）；
2. 交付包存放在独立仓库还是仓库内 `integrations/`，如何保证默认构建零 runtime；
3. 真实能力如何接入 M5 评测（权重来源、冒烟路径与真实模型的边界）；
4. 性能数字的归因口径（避免把 runtime 体积与耗时错误归因于 Core，设计 §20）。

## 决策

### 1. 主选 ncnn，ONNX Runtime 为文档化备选

选型对比（工程规范 §9.3 口径，2026-09-15 复核）：

| 维度 | ncnn | ONNX Runtime | MNN | TensorRT |
| --- | --- | --- | --- | --- |
| 许可证 | BSD-3-Clause | MIT | Apache-2.0 | 专有（NVIDIA EULA），排除 |
| 维护状态 | 腾讯持续维护，pnnx 转换链活跃 | 微软持续维护，2026-09 仍有发布 | 阿里维护，本案生态适配较弱 | 随 NVIDIA SDK 节奏 |
| 目标平台 | Android NDK / Linux / Windows，无第三方运行时依赖 | Android NDK（.so/AAR）/ Linux / Windows | Android / Linux / Windows | 仅 NVIDIA GPU 平台 |
| 体积影响 | 核心静态库 1-2 MiB 量级，可选 Vulkan | 库级 10 MiB 以上（CPU EP，可裁剪） | 小到中 | 大且绑定 GPU |
| 模型链路 | pnnx / onnx2ncnn；PP-OCR、YOLO 均有成熟 ncnn 移植 | 原生 ONNX；paddle2onnx 官方链路，PP-OCR ONNX 生态成熟 | 转换链较弱 | 需离线引擎编译 |

主选 **ncnn**，理由：

- 与 Mirador「轻量终端」定位直接匹配：无第三方运行时依赖、体积最小、移动端 CPU
  路径成熟，符合设计 §20「避免工作优先、不引入失控资源」的取向；
- BSD-3-Clause 许可证干净，与仓库既有 googletest 同类；且交付包不随核心分发
  （见下），许可证义务实际落在 integrations 的使用者侧，核心仓库仅记录审查结论；
- 三平台同一套 C++ 代码路径，M5 CI 可对 NDK/MSVC 做真实编译验证，而不是只验证
  单平台可用；
- Mirador 已在 M3 交付 letterbox、NMS、DB 后处理、行合并等通用组件（`DEC-014`），
  runtime 只需承担网络 forward——ncnn 的最小 C++ API 面恰好匹配这一分工，适配层
  代码量最小。

**ONNX Runtime 为文档化备选**：若 ncnn 转换链在 PP-OCR/YOLO 特定算子上遇阻（触发
条件：M5-03/M5-04 实施中确认某算子无 ncnn 实现且 pnnx 无法转换），按本决策的备选
路径切换到 ONNX Runtime，无需重新立项；Backend SPI（`DEC-012`）已把 runtime 隔离
在适配层内，切换成本被接口边界保护。

### 2. 存放形式：仓库内 `integrations/`，默认构建零获取

- 参考后端交付包位于本仓库 `integrations/`（`integrations/ocr_ppocr`、
  `integrations/detector_yolo`、共享的 `integrations/common`），不新建独立仓库：
  保持与核心 SPI 演进同步评审，且 M5 评测接入路径最短。
- 新增 CMake 选项 `MIRADOR_BUILD_INTEGRATIONS`，**默认 OFF**：默认构建（含 CI 常规
  矩阵、最小核心构建）不配置、不下载、不编译任何 runtime；`integrations/` 不进入
  核心安装与发布包，不随核心版本号发布（总计划 POST-05 边界）。
- runtime 引入采用 configure 阶段 FetchContent 拉取 pinned commit，锁定信息登记于
  `integrations/` 内的锁文件并在 `docs/supply-chain/` 建立审计记录；**不使用
  submodule**，保证默认克隆的源码树零 runtime 内容。引入时走独立 MR
  （工程规范 §9.2.5），同步 `THIRD_PARTY_NOTICES` 的 integrations 分节说明。
- 架构测试同步：默认构建图的链接闭包不允许出现 runtime 令牌；`integrations/`
  源码允许包含 ncnn 头，但不得反向被 `src/`、`include/` 引用。

### 3. 评测接入：权重由使用者显式提供，冒烟与真实模型分层

- 参考后端实现核心 `OcrBackend` / `DetectorBackend` SPI（`DEC-012`），权重与模型
  文件一律由使用者显式路径提供，不进仓库、不下载（`RULE-10` 与工程规范 §9.2.4）。
- 测试分两层：**冒烟层**使用仓库内确定性生成的合成 tiny 模型（手工构造的极小
  param/bin），验证适配层「forward → Mirador 原始结果契约」链路，可进常规 CI
  （仅在 integrations 选项开启的专用 job）；**真实模型层**在 M5 评测阶段由负责人
  在本机提供 PP-OCR mobile / YOLO 权重运行，产出评测数字，仓库只记录方法与结果、
  不保存权重。
- CI 增加一个可选 `integrations` job（选项 ON）：FetchContent 拉取 ncnn、构建
  冒烟层并运行；默认矩阵 job 不受影响。

### 4. 性能归因口径

M5 基准（`SCOPE-08`）报告时，Core 外层开销（变化检测、缓存命中路径、Backend 调用
外层耗时）与 ncnn/模型的端到端耗时**分开列报**，runtime 体积与耗时不得计入 Core；
数字标注环境（`DEC-011`）与证据等级（工程规范 §7）。

## 备选方案

- **ONNX Runtime 为主选**：生态转换链最顺（PP-OCR ONNX 成熟）、MIT 干净；但库体积
  大一个量级、Android 侧以预编译 .so/AAR 为主（CI 全源码验证成本高），与「轻量」
  定位的匹配度低于 ncnn；保留为备选而非主选。
- **独立仓库存放交付包**：隔离最彻底，但 SPI 仍在快速演进期（M4 刚冻结），跨仓库
  同步评审成本高；待 SPI 稳定后可整体迁出，本决策不排除未来迁移。
- **MNN 为主选**：体积与定位同样匹配，但本案目标模型（PP-OCR/YOLO）的转换与移植
  生态弱于 ncnn，落地风险更高；不做主选。
- **TensorRT**：专有许可且平台绑定，直接排除。

## 影响与风险

- `integrations/` 成为新的顶层目录类别：AGENTS.md 边界条款（runtime 只进
  `integrations/` 或独立仓库）由此首次落地，架构测试与 CI 需同步扩展（`M5-02`）。
- ncnn 版本在 `M5-02` 引入时 pin 到精确 commit 并登记；后续升级走独立 MR 与审计
  记录（工程规范 §9）。
- `RISK-2026-12`（新增）：ncnn 转换链对 PP-OCR/YOLO 特定算子的覆盖存在不确定性 —
  处置：备选路径已冻结（ONNX Runtime），触发条件见决策第 1 节；不静默扩写自定义
  ncnn 层。
- 评测真实数字依赖权重与设备的可得性，受限时按工程规范第 4 节记录补跑条件，
  不提前宣称性能结论（`DOD-05`）。

## 验证方式

`M5-02`：默认构建图架构测试证明零 runtime；integrations 专用 CI job 在选项 ON 时
拉取 pinned ncnn、构建并运行合成模型冒烟测试。`M5-03`/`M5-04`：冒烟层通过 +
真实模型评测按 `DEC-011` 环境记录。`M5-06`：基准报告按本决策第 4 节归因口径发布。

## 关联文档和工作项

[设计文档](../design/mirador-development-design.md) §9（无 runtime 设计）、§20（性能
归因）、§21（integrations 存放）、§24 M5；总计划 `POST-05`、`SCOPE-08`、`SCOPE-10`；
`DEC-012`（Backend SPI 契约）、`DEC-014`（通用组件复用）、[DEC-011](DEC-011-benchmark-environments.md)（基准
环境）；`M5-01`~`M5-06`。
