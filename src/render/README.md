# src/render

`mirador::render` 模块实现目录（M4，链接恰为 `mirador::fusion`，`DEC-013`）：

- `set_of_mark.cpp`：`render_set_of_mark`——把 rotation k0 的单平面 8 位背景复制到
  自有 RGB8 `MarkedImage`，按 stable_id 调色板描框、确定性数字标签芯片与遮挡规避，
  输出 `mark_id -> stable_id` 映射（设计 §17；不调用 VLM、不执行动作）。
- `grid_partition.cpp`：通用网格划分与坐标回映纯几何工具（定位/逆映射/边缘格裁剪）；
  像素裁剪仍由调用方经 `mirador::image` 完成。

公共 API 位于 `include/mirador/`。
