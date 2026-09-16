# 评测集场景清单与离线数据接入约定（M5-06，`SCOPE-08`）

> 状态：Active
> 日期：2026-09-15
> 负责人：linductor
> 依据：[设计文档](../design/mirador-development-design.md) §23；[DEC-011](../decisions/DEC-011-benchmark-environments.md)

本文定义 Mirador 评测集的场景清单（设计 §23 要求的覆盖面）与离线数据的接入
约定。**数据本身不入仓**：截图/录屏与标注由负责人在本地或评测环境提供，仓库只
固定场景标识、覆盖要求与格式约定，保证评测可复现且不携带真实用户内容（`RULE-10`）。

## 场景清单

每个场景有稳定 ID（`<platform>-<scene>`），评测报告按 ID 归组。设计 §23 的
最低覆盖如下；"必须"级别的场景缺失时，对应能力的评测结论必须限定范围。

| 场景 ID | 覆盖点 | 级别 | 关联指标 |
| --- | --- | --- | --- |
| `*-static-page` | 静态页面（连续多帧无变化） | 必须 | 变化检测误检率、缓存命中率、快照复用 |
| `*-partial-anim` | 局部动画（光标闪烁、spinner、视频小窗） | 必须 | 变化 ROI 定位、局部失效正确性 |
| `*-scroll` | 滚动（列表/网页内容位移） | 必须 | 稳定 ID 延续率、缓存淘汰动态 |
| `*-dialog` | 弹窗出现/消失 | 必须 | 融合增量、generation 递增、陈旧区域拒绝 |
| `*-theme-switch` | 明暗主题切换 | 必须 | 指纹失效与重填充、视觉索引误命中 |
| `*-resolution-rotation` | 分辨率变化与 0/90/180/270 旋转 | 必须 | 坐标恢复（`DOD-03` 矩阵）、kDisplay 变换（`DEC-016`） |
| `*-small-icons` | 小图标（≤ 24 px） | 必须 | 视觉索引命中率、crop-refine 收益 |
| `*-similar-icons` | 相似图标（同形异义） | 必须 | 视觉索引区分度（误命中即失败口径） |
| `android-compose` / `android-webview` / `android-flutter` / `android-react-native` | 混合渲染栈 | 按评测目标 | OCR/Accessibility 覆盖率差异 |
| `android-canvas-game` | 自绘/游戏画面（无 Accessibility 节点） | 按评测目标 | 纯视觉路径（无外部区域）的召回 |
| `*-mixed-text` | 中英文混排文本 | 必须 | OCR bbox IoU、文本规范化 |
| `linux-x11-window` / `windows-desktop` | 桌面窗口采集链路 | 必须（`DEC-011` 补跑前限 Linux） | 采集适配 + kDisplay 变换端到端 |

平台维度遵循 `DEC-011`：Linux x64 为主基准环境；物理 Android/Windows 场景在
补跑条件满足前只做 CI 编译级验证，评测结论限定平台。

## 离线数据接入约定

1. **存放**：评测数据放在仓库外的调用方目录（建议 `~/mirador-eval/<scene-id>/`），
   通过路径显式传给评测工具；`benchmarks/` 与 `tests/` 不得引用任何仓库外固定
   绝对路径，离线工具对缺失数据必须显式报错而不是静默跳过。
2. **目录布局**：每个场景目录含 `frames/`（按 `frame_%05d.png` 顺序编号的原始
   截图）、`manifest.json`（见下）、可选 `labels.json`（人工标注，语义区域
   bounds + 文本 + 角色）。
3. **manifest.json 最小字段**：`scene_id`、`platform`、`width`、`height`、
   `rotation`（0/90/180/270）、`frame_count`、`fps_hint`（采集帧率提示）、
   `notes`（已知动态区域等）。字段缺失即数据无效。
4. **隐私**（`RULE-10`）：入库的只有合成数据；真实截图必须来自负责人自有的
   测试账号/页面，不得包含他人个人信息；标注文件不得内嵌像素数据。评测工具
   沿用库默认——内存处理、不落盘、日志脱敏。
5. **确定性**：同一 `scene_id` 的数据文件不可变；要改场景就换新 ID（追加
   版本后缀，如 `linux-static-page-v2`），保证历史评测报告可追溯。

## 几何区域 Proposal 真实数据接入（M6-04）

几何区域 Proposal 实验（`DEC-017`）的真实截图评估沿用上方离线数据接入约定，
并补充以下口径：

1. **场景复用**：真实数据按 `<platform>-<scene>` 场景 ID 组织（如
   `android-mixed-text`、`linux-x11-window`），不另立场景体系；几何 proposal
   评估使用同一目录布局（`frames/` + `manifest.json` + `labels.json`）。
2. **Ground truth**：`labels.json` 中人工标注的语义区域 bounds 即 recall 的
   GT 实体；不要求标注"装饰性框线"，但鼓励在 `notes` 中列出已知装饰结构，
   便于区分 precision 折损来源。
3. **匹配口径**：与合成 harness 一致——proposal `tight_bounds` 对 GT 实体
   bounds 的 IoU ≥ 0.5 记为覆盖；precision、重复率、ROI 缩减同
   [linux-x64-geometric-proposal-2026-09](linux-x64-geometric-proposal-2026-09.md)
   的定义。评估工具对缺失数据（目录、manifest 字段、labels）显式报错，
   不静默跳过。
4. **显式路径**：评测数据经调用方显式路径传入离线工具；`benchmarks/`、
   `tests/` 不引用仓库外固定路径，真实截图与标注永不入仓（`RULE-10`）。
5. **结论限定**：真实数据结论必须与合成口径分开列报；只有合成证据时，
   go/no-go 判定必须显式记录"仅有合成证据"（`DEC-017`、`RISK-2026-14`）。

## 与指标的对账

场景覆盖支撑设计 §23/§26 的指标口径：变化检测漏检/误检、OCR 文本与 bbox、
区域 proposal recall/precision、稳定 ID 延续率、缓存命中率、端到端 p50/p95、
RSS 与包体积。每份评测报告必须列出所用 `scene_id` 清单与数据 manifest 摘要，
缺失"必须"级场景时显式声明范围限定（同 `DEC-011` 第 4 节格式）。
