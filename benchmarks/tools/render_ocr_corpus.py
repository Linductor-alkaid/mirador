#!/usr/bin/env python3
"""M8-05 corpus generator: renders a deterministic OCR recognition corpus.

Produces the caller-provided corpus for benchmarks/ocr_rec_experiment.cpp
(AC8-09 evidence): desktop-UI-style English/Chinese text lines rendered with a
caller-named font into packed raw RGB8 images, plus a dense synthetic desktop
frame for the end-to-end sanity arm. Text and images stay in the caller's
output directory — nothing enters the Mirador repository (no weights, no
captured frames, no third-party font files).

Requires Pillow (tested with Pillow >= 10). Deterministic under a fixed seed:
the same seed/font/size parameters reproduce the corpus byte-for-byte.

Output layout (in --out):
  lines.tsv    id \t file \t width \t height \t width_class   (width_class:
               0 short, 1 medium, 2 long, 3 overlong — by rendered width)
  line_%04d.rgb  packed RGB8, top-left origin, no stride padding
  dense.tsv    file \t width \t height \t text_count
  dense.rgb    packed RGB8 dense frame (when --dense is set)
  manifest.json  human-readable provenance (texts, font, seed)
"""

import argparse
import json
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# Desktop-UI-style public strings (no user data): window titles, menu labels,
# status lines and short technical sentences in English and Simplified Chinese,
# matching the mixed-script desktop frames the budgeted OCR path targets.
TEXT_POOL = {
    0: [  # short: renders to roughly <= 128 px at the reference size
        "确定", "OK", "取消", "File", "Edit", "设置", "保存", "关闭", "Help",
        "View", "重试", "删除", "Copy", "Paste", "刷新", "返回", "Search",
        "Print", "登录", "Logout", "新建", "Undo", "Home", "Start",
    ],
    1: [  # medium: roughly 128-256 px
        "系统设置", "Network Settings", "用户账户控制", "Preferences",
        "下载完成", "Build passed", "正在同步文件", "Battery 87%",
        "存储空间不足", "Update available", "已连接到网络", "Volume 32%",
        "版本 2.4.1", "CPU 占用 45%", "3 个新通知", "Night mode on",
        "编辑配置文件", "Run tests again", "回收站已清空", "信号强度良好",
    ],
    2: [  # long: roughly 256-320 px at the reference size
        "终端输出: 3 errors, 12 warnings", "镜像源同步完成 (2,431 packages)",
        "任务计划: 每天 09:00 备份文档", "无法连接到打印机 HP-LaserJet",
        "上次登录: 2026-10-19 08:42:17", "CPU 温度 62°C 风速 2400 RPM",
        "代码评审通过: mirador::PerceptionSession", "磁盘 D 剩余 128.5 GB",
        "防火墙已拦截 4 个入站连接", "会议提醒: 15:30 视觉组周会",
    ],
    3: [  # overlong: wider than the 320 px rec cap, so the frozen path squashes
        "编译完成: 214 个目标, 跳过 6 个, 用时 3 分 42 秒",
        "警告: 检测到未保存的更改, 是否在退出前保存所有编辑器窗口?",
        "同步日志: 上传 1,204 个文件, 下载 87 个, 冲突 2 个, 失败 0 个",
        "系统诊断报告: 内存占用 61%, 磁盘占用 74%, 网络延迟 12ms, 丢包率 0%",
        "_RELEASE_NOTES: 修复了窗口管理器在多显示器横向排列时的焦点漂移问题",
        "统计: 本周新增任务 23 个, 完成 18 个, 进行中 4 个, 已取消 1 个",
    ],
}

DENSE_ROW_STEP = 36
DENSE_COL_STEP = 120
DENSE_MARGIN = 12


def pick_sc_face(path: str) -> int:
    """Returns the TrueType collection index of the Simplified Chinese face."""
    for index in range(16):
        try:
            probe = ImageFont.truetype(path, 16, index=index)
        except OSError:
            break
        name = " ".join(probe.getname())
        if "SC" in name:
            return index
    return 0  # fall back to the first face; recorded in the manifest


def render_line(text: str, font: ImageFont.FreeTypeFont, height: int, rng: random.Random):
    """Renders one text line into a packed raw RGB8 buffer (dark on light)."""
    probe = Image.new("RGB", (8, 8))
    draw = ImageDraw.Draw(probe)
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    width = (right - left) + 8
    image = Image.new("RGB", (width, height), (250, 250, 250))
    draw = ImageDraw.Draw(image)
    color = (28, 28, 32) if rng.random() < 0.85 else (40, 60, 130)
    draw.text((4 - left, (height - (bottom - top)) // 2 - top), text, font=font, fill=color)
    return image


def image_to_rgb_bytes(image: Image.Image) -> bytes:
    return image.convert("RGB").tobytes()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="output directory (created)")
    parser.add_argument("--font", required=True, help="TTF/TTC font path, caller-provided")
    parser.add_argument("--font-index", type=int, default=-1,
                        help="TTC face index; -1 auto-detects the SC face")
    parser.add_argument("--font-size", type=int, default=36)
    parser.add_argument("--canvas-height", type=int, default=48,
                        help="line canvas height (the rec unit resizes to this)")
    parser.add_argument("--per-class", type=int, default=24, help="lines per width class")
    parser.add_argument("--dense", action="store_true", help="also render the dense frame")
    parser.add_argument("--seed", type=int, default=20261019)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)

    index = args.font_index
    if index < 0:
        index = pick_sc_face(args.font)
    font = ImageFont.truetype(args.font, args.font_size, index=index)
    face_name = " ".join(font.getname())

    manifest_lines = []
    lines_rows = []
    per_class = {klass: [] for klass in TEXT_POOL}
    for klass, pool in TEXT_POOL.items():
        picks = pool[:]
        rng.shuffle(picks)
        for i in range(args.per_class):
            text = picks[i % len(picks)]
            if i >= len(picks):
                # Widen the pick space for large per-class counts by appending
                # a deterministic counter to repeated strings.
                text = f"{text} {i // len(picks) + 1}"
            per_class[klass].append(text)

    line_id = 0
    for klass in sorted(TEXT_POOL):
        for text in per_class[klass]:
            image = render_line(text, font, args.canvas_height, rng)
            name = f"line_{line_id:04d}.rgb"
            (args.out / name).write_bytes(image_to_rgb_bytes(image))
            lines_rows.append(f"{line_id}\t{name}\t{image.width}\t{image.height}\t{klass}")
            manifest_lines.append(
                {"id": line_id, "file": name, "text": text, "width_class": klass,
                 "width": image.width, "height": image.height}
            )
            line_id += 1

    (args.out / "lines.tsv").write_text("\n".join(lines_rows) + "\n", encoding="utf-8")

    manifest = {
        "generator": "render_ocr_corpus.py",
        "seed": args.seed,
        "font": args.font,
        "font_face": face_name,
        "font_index": index,
        "font_size": args.font_size,
        "canvas_height": args.canvas_height,
        "lines": manifest_lines,
    }

    if args.dense:
        cells = []
        y = DENSE_MARGIN
        while y + args.canvas_height <= 1000 - DENSE_MARGIN:
            x = DENSE_MARGIN
            while x < 1600 - DENSE_MARGIN - 100:
                cells.append((x, y))
                x += DENSE_COL_STEP
            y += DENSE_ROW_STEP
        image = Image.new("RGB", (1600, 1000), (247, 247, 249))
        draw = ImageDraw.Draw(image)
        dense_font = ImageFont.truetype(args.font, 24, index=index)
        short_pool = TEXT_POOL[0][:]
        rng.shuffle(short_pool)
        for count, (x, yy) in enumerate(cells):
            text = short_pool[count % len(short_pool)]
            draw.text((x, yy + 6), text, font=dense_font, fill=(35, 35, 40))
        (args.out / "dense.rgb").write_bytes(image_to_rgb_bytes(image))
        (args.out / "dense.tsv").write_text(f"dense.rgb\t1600\t1000\t{len(cells)}\n", encoding="utf-8")
        manifest["dense"] = {"file": "dense.rgb", "width": 1600, "height": 1000, "count": len(cells)}

    (args.out / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"corpus: {len(manifest_lines)} lines in {args.out} "
          f"(font face '{face_name}', index {index}, seed {args.seed})")
    if args.dense:
        print(f"dense frame: {manifest['dense']}")


if __name__ == "__main__":
    main()
