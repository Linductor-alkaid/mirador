#!/usr/bin/env python3
"""M8-06 scenario generator: renders readable OCR benchmark scenes.

Produces the caller-provided readable scenes for benchmarks/ocr_budget_bench.cpp
--scene-dir (ocr-budget-protocol.md section 1 scenario IDs): desktop-UI-style
English/Simplified-Chinese text laid out on a light canvas and rendered with a
caller-named font into one packed raw RGB8 image plus a TSV manifest of the
drawn line boxes (ground-truth geometry for the harness gates, including the 16
designated caller-priority lines). Scenes stay in the caller's output directory
— nothing enters the Mirador repository (no weights, no captured frames, no
third-party font files).

Requires Pillow (tested with Pillow >= 10). Deterministic under a fixed seed:
the same seed/font/layout parameters reproduce every file byte-for-byte.

Output layout (in --out):
  scene.tsv    width \t height \t line_count   (single row)
  lines.tsv    id \t x \t y \t w \t h \t priority \t text
               (drawn-text bounding box in image pixels; priority is 1 for the
               16 designated caller-priority lines, else 0; text is UTF-8
               without tabs/newlines)
  scene.rgb    packed RGB8, top-left origin, no stride padding
  manifest.json  provenance (scenario, font, seed, layout parameters, texts)
"""

import argparse
import json
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# Desktop-UI-style public strings (no user data), shared with the M8-05 corpus
# generator conventions: window labels, status lines and short technical
# sentences in English and Simplified Chinese.
SHORT_POOL = [
    "确定", "OK", "取消", "File", "Edit", "设置", "保存", "关闭", "Help",
    "View", "重试", "删除", "Copy", "Paste", "刷新", "返回", "Search",
    "Print", "登录", "Logout", "新建", "Undo", "Home", "Start",
]
MEDIUM_POOL = [
    "系统设置", "Network Settings", "用户账户控制", "Preferences",
    "下载完成", "Build passed", "正在同步文件", "Battery 87%",
    "存储空间不足", "Update available", "已连接到网络", "Volume 32%",
    "版本 2.4.1", "CPU 占用 45%", "3 个新通知", "Night mode on",
]
LONG_POOL = [
    "终端输出: 3 errors, 12 warnings", "镜像源同步完成 (2,431 packages)",
    "任务计划: 每天 09:00 备份文档", "无法连接到打印机 HP-LaserJet",
    "上次登录: 2026-10-19 08:42:17", "CPU 温度 62°C 风速 2400 RPM",
    "代码评审通过: mirador::PerceptionSession", "磁盘 D 剩余 128.5 GB",
    "防火墙已拦截 4 个入站连接", "会议提醒: 15:30 视觉组周会",
    "编译完成: 214 个目标, 跳过 6 个, 用时 3 分 42 秒",
    "警告: 检测到未保存的更改, 是否在退出前保存所有编辑器窗口?",
]
PRIORITY_LINES = 16

DEFAULTS = {
    "dense359": {"cols": 19, "rows": 19, "count": 359, "col_step": 82, "row_step": 34,
                 "font_size": 17, "margin": 16},
    "dense512": {"cols": 23, "rows": 23, "count": 529, "col_step": 68, "row_step": 34,
                 "font_size": 16, "margin": 12},
    "sparse": {"cols": 8, "rows": 13, "count": 0, "col_step": 190, "row_step": 70,
               "font_size": 20, "margin": 40},
    "longlines": {"cols": 1, "rows": 10, "count": 10, "col_step": 1520, "row_step": 92,
                  "font_size": 24, "margin": 40},
}


def pick_sc_face(path: str) -> tuple:
    """Returns (TrueType collection index of the SC face, face name)."""
    for index in range(16):
        try:
            probe = ImageFont.truetype(path, 16, index=index)
        except OSError:
            break
        name = " ".join(probe.getname())
        if "SC" in name:
            return index, name
    probe = ImageFont.truetype(path, 16)
    return 0, " ".join(probe.getname())  # fall back; recorded in the manifest


def fit_text(draw, text, max_width, font):
    """Returns the text only when its drawn width fits `max_width`."""
    left, _, right, _ = draw.textbbox((0, 0), text, font=font)
    return text if (right - left) <= max_width else None


def pick_line_text(draw, rng, slot_width, font):
    """Deterministically picks the widest pool class that fits the slot."""
    order = [LONG_POOL, MEDIUM_POOL, SHORT_POOL] if slot_width > 340 else [MEDIUM_POOL, SHORT_POOL]
    for pool in order:
        candidates = pool[:]
        rng.shuffle(candidates)
        for text in candidates:
            fitted = fit_text(draw, text, slot_width - 8, font)
            if fitted is not None:
                return fitted
    return SHORT_POOL[0]  # unreachable with the reference fonts; deterministic


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", required=True, choices=sorted(DEFAULTS))
    parser.add_argument("--out", type=Path, required=True, help="output directory (created)")
    parser.add_argument("--font", required=True, help="path to a CJK-capable TTF/TTC (caller-provided)")
    parser.add_argument("--seed", type=int, default=None,
                        help="deterministic seed (default: scenario name hash)")
    parser.add_argument("--sparse-n", type=int, default=32, help="line count for --scenario sparse")
    parser.add_argument("--cols", type=int, default=None, help="override grid columns")
    parser.add_argument("--rows", type=int, default=None, help="override grid rows")
    parser.add_argument("--count", type=int, default=None,
                        help="override the number of drawn lines (first slots in reading order)")
    parser.add_argument("--col-step", type=int, default=None, help="override column step")
    parser.add_argument("--row-step", type=int, default=None, help="override row step")
    parser.add_argument("--font-size", type=int, default=None, help="override font size")
    parser.add_argument("--margin", type=int, default=None, help="override canvas margin")
    args = parser.parse_args()

    layout = dict(DEFAULTS[args.scenario])
    if args.cols is not None:
        layout["cols"] = args.cols
    if args.rows is not None:
        layout["rows"] = args.rows
    if args.col_step is not None:
        layout["col_step"] = args.col_step
    if args.row_step is not None:
        layout["row_step"] = args.row_step
    if args.font_size is not None:
        layout["font_size"] = args.font_size
    if args.margin is not None:
        layout["margin"] = args.margin
    count = layout["cols"] * layout["rows"]
    if args.scenario == "sparse":
        count = args.sparse_n
    elif args.count is not None:
        count = args.count
    if count > layout["cols"] * layout["rows"]:
        parser.error(f"count {count} exceeds the {layout['cols']}x{layout['rows']} slot grid")

    width, height = 1600, 1000
    seed = args.seed if args.seed is not None else (
        {"dense359": 20261106, "dense512": 20261107, "sparse": 20261108, "longlines": 20261109}[args.scenario])
    rng = random.Random(seed)

    index, face_name = pick_sc_face(args.font)
    font = ImageFont.truetype(args.font, layout["font_size"], index=index)

    image = Image.new("RGB", (width, height), (247, 247, 249))
    draw = ImageDraw.Draw(image)
    lines = []
    for slot in range(count):
        col = slot % layout["cols"]
        row = slot // layout["cols"]
        slot_x = layout["margin"] + col * layout["col_step"]
        slot_y = layout["margin"] + row * layout["row_step"]
        slot_width = min(layout["col_step"] - 8, width - layout["margin"] - slot_x)
        text = pick_line_text(draw, rng, slot_width, font)
        color = (35, 35, 40) if rng.random() < 0.85 else (40, 60, 130)
        left, top, right, bottom = draw.textbbox((slot_x, slot_y), text, font=font)
        draw.text((slot_x - left, slot_y - top), text, font=font, fill=color)
        priority = 1 if (count >= PRIORITY_LINES and slot % (count // PRIORITY_LINES) == 0) else 0
        lines.append({"id": slot, "x": int(left), "y": int(top), "w": int(right - left),
                      "h": int(bottom - top), "priority": priority, "text": text})

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "scene.rgb").write_bytes(image.convert("RGB").tobytes())
    (args.out / "scene.tsv").write_text(f"{width}\t{height}\t{len(lines)}\n", encoding="utf-8")
    with (args.out / "lines.tsv").open("w", encoding="utf-8") as manifest:
        for line in lines:
            manifest.write(
                f"{line['id']}\t{line['x']}\t{line['y']}\t{line['w']}\t{line['h']}\t"
                f"{line['priority']}\t{line['text']}\n")
    (args.out / "manifest.json").write_text(
        json.dumps({
            "scenario": args.scenario,
            "sparse_n": args.sparse_n if args.scenario == "sparse" else None,
            "seed": seed,
            "font": args.font,
            "font_face": face_name,
            "font_index": index,
            "font_size": layout["font_size"],
            "layout": {key: layout[key] for key in ("cols", "rows", "col_step", "row_step", "margin")},
            "line_count": len(lines),
            "priority_lines": sum(line["priority"] for line in lines),
            "lines": lines,
        }, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    priorities = sum(line["priority"] for line in lines)
    print(f"scene: {args.scenario} {width}x{height} lines={len(lines)} priority={priorities} "
          f"seed={seed} font='{face_name}' index={index} -> {args.out}")
    if count > 0 and priorities == 0:
        print("note: line count too small for the 16 designated priority lines")


if __name__ == "__main__":
    main()
