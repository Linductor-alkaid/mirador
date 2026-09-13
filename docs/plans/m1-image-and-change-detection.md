# M1：基础图像与变化检测

> 状态：In Progress
> 负责人：linductor
> 所属计划：[Mirador 实施总计划](mirador-implementation-plan.md)
> 前置：M0
> 建议发布点：`v0.1.0-beta.1`
> 更新日期：2026-09-14

## 目标

在 `mirador::image` 模块落地 CPU 基础图像操作与画面变化检测的首个确定性实现：颜色转换、
裁剪、面积缩放、dHash 指纹、分块差分与变化 ROI、帧级有界缓存；提供 `cv::Mat` ↔
`ImageView` 可选适配与无 runtime 示例；完成不变画面、局部变化、旋转和动态区域忽略四场景
基准，为"低负载"建立首个可量化闭环（设计文档 §11、§24 M1）。

## 范围与非目标

范围：`mirador::image` 从 INTERFACE 目标变为编译目标并承载以上操作；`RectI` 等最小
公共类型补充；架构测试扩展到 image 模块依赖方向；基准入口与示例。
非目标：Backend SPI 与请求/结果类型（M2）、能力结果缓存与语义快照缓存（M2/M3）、
`PerceptionSession` 会话状态机（M2 起）、稳定时间窗/滞回的跨帧有状态策略（随会话模型
落地）、光流/特征增强（`POST-03` 触发）、线段检测（M3）。

## 设计与决策依据

- [设计文档](../design/mirador-development-design.md) §11（变化检测）、§12（帧级缓存层）、
  §20（预算与保护）、§23（测试与评测）、§24 M1。
- 已生效：`DEC-001`（同步 API）、`DEC-002`（Core 不链接模型 runtime）、`DEC-003`（公共
  API 不暴露 OpenCV 类型）、`DEC-004`（`Result<T>`/`Status`）、`DEC-005`（构建基线）、
  `DEC-006`（GoogleTest）。
- 待冻结：`DEC-007`（NV12 多平面表示）在颜色转换与差分实际消费平面数据后冻结；
  `DEC-008`（缓存默认字节预算，暂定帧 4 MiB）最迟 M2，本里程碑按暂定值实现。

## 工作项

- [x] `M1-01` `mirador::image` 编译目标与有界拥有缓冲 `ImageBuffer`：预算参数显式、
  分配前校验、失败返回 `kBudgetExceeded`、`view()` 输出合法视图；架构测试锁住
  image → core 单一依赖方向。
- [x] `M1-02` 确定性颜色转换 `convert_color`：文档化支持矩阵（同格式行拷贝、彩色→灰度、
  灰度→彩色、交错格式互转、NV12→彩色/灰度；彩色→NV12 返回 `kUnsupportedFormat`）；
  BT.601 全范围整数定点系数，像素循环无浮点，跨编译器位稳定；stride 感知。
- [x] `M1-03` `RectI` 像素整数矩形（core：构造校验、`intersect`/`contains`，int64 防溢出）
  与旋转感知裁剪 `crop`：0/90/180/270 方向、非连续 stride、奇数尺寸、NV12 色度取整规则、
  越界 ROI 返回 `kInvalidArgument` 的测试矩阵。
- [x] `M1-04` 面积重采样 `resize_area`：任意比例 box 权重整数实现（含非整数比例与放大）、
  灰度/交错格式同格式缩放、NV12→灰度仅亮度路径；同内容不同 stride 输出一致；缩放坐标
  与 `Transform2D::make_scale` 往返一致。
- [x] `M1-05` dHash 指纹：9×8 灰度 64 位差分哈希、汉明距离与相似度；`fingerprint()`
  组合入口（内部缓冲有界，接受全部六种格式）；同内容不同布局指纹相同、不同内容区分度
  测试。
- [ ] `M1-06` `ChangeReport` 与 `detect_change` 分层变化检测：指纹早退 → 缩略灰度分块
  差分 → 相邻块合并为变化 ROI 并回映原图坐标；输出帧相似度、变化面积比例、ROI 列表、
  阈值与分类（无变化/局部/全局）；支持忽略区域；不变画面零 ROI、局部变化 ROI 精确、
  旋转分类为全局的测试矩阵。
- [ ] `M1-07` `FrameCache` 帧级有界缓存：字节预算、LRU 淘汰、单条目超预算显式
  `kBudgetExceeded`、键替换与失效路径测试（不只验证命中）。
- [ ] `M1-08` 变化检测基准入口（`benchmarks/`）：不变画面、局部变化、旋转、动态区域忽略
  四场景，确定性合成帧，输出 p50/p95；本机（Linux）首个数字写入验证记录，不做跨平台
  性能声明。
- [ ] `M1-09` OpenCV 适配（`adapters/opencv`，可选依赖默认关闭）：`cv::Mat` →
  `ImageView` 包装与有界缓冲导出；公共 API 与 `src/` 不出现 OpenCV 类型；在具备 OpenCV
  的环境完成构建与测试。
- [ ] `M1-10` 无 runtime 示例与文档同步：示例使用公共 API 演示"提交帧 → 变化检测 →
  ROI/指纹输出"并纳入编译验证；`DEC-007` 冻结、README 模块状态、CHANGELOG 更新。

## 风险与阻塞

- `RISK-2026-01`：MSVC/NDK 环境可得性受限时，跨平台编译证据依赖 CI，受限时按规范记录
  补跑条件。
- `RISK-2026-05`（新增）：像素循环若引入浮点或未指定行为，跨编译器位确定性会被破坏。
  处置：颜色转换与重采样全部使用整数定点并经 UBSAN；确定性以测试锁定。
- `RISK-2026-06`（新增）：本机无 OpenCV 环境时 `M1-09` 无法本地验证。处置：保持工作项
  未勾选并记录补跑条件，不阻塞其余项。
- `DEC-007` 未冻结会阻塞 NV12 消费路径的最终语义；按计划在 `M1-02` 落地后冻结。

## 测试与退出条件

- [ ] 全部 6 个 Linux 预设（debug/release/warnings/asan/ubsan/tsan）配置、构建、ctest
  通过；`clang-format`/`clang-tidy` 无告警。
- [ ] 颜色转换、裁剪、缩放测试矩阵覆盖 0/90/180/270 方向、非连续 stride、奇数尺寸与
  往返一致；不支持格式组合显式报 `kUnsupportedFormat`。
- [ ] 指纹稳定性：相同像素内容经不同 stride/偏移布局指纹一致；相邻内容有可预期区分度。
- [ ] 变化检测：不变画面输出零 ROI 且相似度为 1；局部变化 ROI 覆盖变化块；旋转/全局
  变化分类为全局；忽略区域内的动画不产生失效。
- [ ] 帧缓存：字节预算强制、LRU 淘汰顺序、单条目超限显式错误、键覆盖更新失效路径。
- [ ] 架构测试扩展后：image 目标链接闭包仅 core + 标准库；公共头与 `src/` 无第三方
  令牌；CI 运行。
- [ ] 基准产出本机数字（环境、命令、p50/p95），性能声明限定在该环境。
- [ ] `DEC-007` 在 NV12 消费路径落地后冻结为 Accepted；总计划与本里程碑文档同步。

## 验证记录

2026-09-14：里程碑创建。依据设计文档 §11/§24 M1 与总计划 `SCOPE-02` 拆分工作项
`M1-01`~`M1-10`；`DEC-008` 按暂定值（帧 4 MiB）进入实现，冻结仍留在 M2。

2026-09-14：`M1-01`~`M1-03` 实施完成（分支 `feat/image-m1-image-ops`，commit
`03e858c`..本节对应提交）。

- 环境：Ubuntu 24.04 x64（GCC 13.3.0、CMake 3.28.3 + Ninja、clang-format/clang-tidy
  18.1.3）。
- 落地内容：
  - `M1-01`：`mirador::image` 转编译目标 + `ImageBuffer`（显式字节预算、预算/分配双路径
    `kBudgetExceeded`、DEC-007 平面布局、`view()` 投影）；架构测试新增"每个一方模块的
    链接接口恰为 `mirador::core`"断言与 image 链接闭包探针（`readelf` NEEDED）；模块
    关闭路径（`MIRADOR_BUILD_IMAGE=OFF`）实测目标消失、构建绿。
  - `M1-02`：`convert_color` 文档化支持矩阵（同格式拷贝、彩色→灰度 BT.601 整数公式、
    灰度→彩色、交错互转、NV12→彩色/灰度；→NV12 显式 `kUnsupportedFormat`），纯整数
    像素循环，分配前预算校验。
  - `M1-03`：core `RectI`（`is_valid`/`contains`×2/`intersect`，int64 边缘运算）与
    `crop`（呈现空间 ROI、NV12 色度子采样、越界/空 ROI/预算错误）。
- **`DEC-007` 冻结为 Accepted**：M0 草案"NV12 色度行 = width 字节"在奇数宽度下无法
  容纳末尾 V 分量，冻结时修正为 `width + width % 2` 并同步 `pixel_format`/`ImageBuffer`/
  转换/裁剪；`tests/core/image_view_test.cpp` 中 M0 期望值（`kNv12,1,7 == 7`）重新基线
  为 `8`，属缺陷修正而非语义变更，已记录于 `DEC-007` 第 5 条。
- 本地命令与结果：`cmake --preset debug` + build + `ctest` 12/12 通过（unit 8、
  property 1、architecture 3）；touched 文件 `clang-format --dry-run --Werror` 与
  `clang-tidy --warnings-as-errors='*'` 无告警。全预设矩阵在里程碑收尾统一补跑。
- 限制：跨平台编译证据待 CI 运行回填。

2026-09-14：`M1-04`、`M1-05` 实施完成与阶段验证（分支 `feat/image-m1-image-ops`，
commit `a928a1b`（含 amend）、`e0cd815`、`53710c5`）。

- 落地内容：
  - `M1-04`：`resize_area` 精确面积权重（按目标尺寸放大的整数覆盖率、半向上取整、
    同尺寸行拷贝快路径），NV12 仅亮度路径输出灰度缩略图；`Transform2D::make_scale`
    往返一致测试覆盖坐标恢复（RULE-05）。
  - `M1-05`：`dhash_9x8`（严格 9×8 灰度、位序定义固定）、`hamming_distance`
    （可移植 SWAR popcount）、`fingerprint_similarity` 与有界组合入口
    `fingerprint()`；stride 无关稳定性、反相内容距离 64、3 像素平移相似度 ≥0.7、
    灰度/NV12 同内容同指纹。
- 阶段验证（本地，Ubuntu 24.04 x64，GCC 13.3.0、CMake 3.28.3 + Ninja、
  clang-format/clang-tidy 18.1.3）：
  - 全部 6 个 Linux 预设 configure + build + `ctest` 14/14 通过（tsan 经
    `setarch "$(uname -m)" -R`）；变更文件 `clang-format --dry-run --Werror` 与
    `clang-tidy --warnings-as-errors='*' -p build/debug` 无告警。
  - ASAN 抓获两处测试侧 NV12 色度平面越界（4×2 色度仅 1 行、8×8 色度仅 32 字节，
    测试写超），已修正为按分配平面字节数写入；属测试缺陷，实现无越界。
  - 架构测试在本阶段实际覆盖：`source_scan`（公共头与 src 无第三方/线程令牌）、
    `link_closure`（core）与 `link_closure_image`（image 链接闭包仅标准库）。
- 限制：跨平台编译证据待 CI 运行回填；`M1-01`~`M1-05` 的验收以本地证据先行记录，
  CI 结果合入后补充。

2026-09-14：`M1-01`~`M1-05` 跨平台编译证据回填。GitHub Actions run
`34773477251`（[PR #3](https://github.com/Linductor-alkaid/mirador/pull/3)）9/9 job
success：linux gcc/clang debug、gcc warnings/asan/ubsan/tsan、windows msvc/ninja、
android ndk arm64-v8a（configure+build）、lint。公共头在 GCC、Clang、MSVC 下编译通过，
NDK arm64-v8a 交叉编译通过；架构测试（source_scan、link_closure、link_closure_image）
在全部 job 运行通过。
