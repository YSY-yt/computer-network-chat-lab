#!/usr/bin/env python3
"""Draw a compact CHAT/1 protocol sequence diagram."""

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.font_manager as fm
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, Rectangle

ROOT = Path(__file__).resolve().parent
OUT = ROOT.parent / "images" / "protocol_sequence.png"

FONT_CANDIDATES = [
    Path(r"C:\Windows\Fonts\msyh.ttc"),
    Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
    Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttf"),
]
font_path = next((p for p in FONT_CANDIDATES if p.exists()), None)
if font_path:
    fm.fontManager.addfont(str(font_path))
    plt.rcParams["font.family"] = fm.FontProperties(fname=str(font_path)).get_name()
plt.rcParams["axes.unicode_minus"] = False

BLUE = "#2B5D8C"
GREEN = "#2F7D57"
GRAY = "#6B7280"
LIGHT = "#EAF2FB"

fig, ax = plt.subplots(figsize=(7.8, 8.3))
ax.set_xlim(0, 7.8)
ax.set_ylim(0, 8.5)
ax.axis("off")

people = [("客户端 A\nAlice", 1.05), ("服务器\nTCP Server", 3.75),
          ("客户端 B\nBob", 6.45)]
top = 7.75
bottom = 1.15

for label, x in people:
    ax.add_patch(Rectangle((x - 0.85, top - 0.38), 1.70, 0.66,
                           facecolor=LIGHT, edgecolor=BLUE, linewidth=1.2,
                           zorder=3))
    ax.text(x, top - 0.05, label, ha="center", va="center", fontsize=10.5,
            color="#17324D", zorder=4)
    ax.plot([x, x], [bottom, top - 0.38], color=GRAY,
            linestyle=(0, (4, 4)), linewidth=1.0, zorder=1)

ax.text(3.9, 8.27, "CHAT/1 协议交互时序", ha="center", va="center",
        fontsize=14, fontweight="bold", color="#111827")


def message(x1, x2, y, text, color=BLUE, linestyle="-", fontsize=8.8,
            label_y=None):
    ax.add_patch(FancyArrowPatch((x1, y), (x2, y), arrowstyle="-|>",
                                 mutation_scale=11, linewidth=1.15,
                                 color=color, linestyle=linestyle,
                                 shrinkA=1, shrinkB=1, zorder=5))
    if text:
        ax.text((x1 + x2) / 2, label_y if label_y is not None else y + 0.05,
                text, ha="center", va="bottom", fontsize=fontsize,
                color=color)


def step(y, number):
    ax.text(0.12, y, f"t{number}", ha="left", va="center", fontsize=7.2,
            color="#9CA3AF")


step(7.22, 0)
message(1.05, 3.75, 7.15, "TCP connect（三次握手）", GRAY)
step(6.88, 1)
message(1.05, 3.75, 6.82, "LOGIN (Alice, CHAT/1)", BLUE)
step(6.54, 2)
message(3.75, 1.05, 6.48, "WELCOME #1", GREEN)
step(6.20, 3)
message(3.75, 1.05, 6.14, "JOIN #2 (Alice)", GREEN)
step(5.76, 4)
message(6.45, 3.75, 5.70, "TCP connect + LOGIN (Bob)", BLUE)
message(3.75, 6.45, 5.36, "WELCOME #1", GREEN)
step(5.02, 5)
message(3.75, 1.05, 4.96, "JOIN #3 (Bob)", GREEN)
message(3.75, 6.45, 4.96, "JOIN #2 (Bob)", GREEN,
        label_y=4.68, fontsize=8.2)
step(4.52, 6)
message(1.05, 3.75, 4.46, "CHAT: Hello 你好，Bob", BLUE)
step(4.18, 7)
message(3.75, 1.05, 4.12, "CHAT #4", GREEN)
message(3.75, 6.45, 4.12, "CHAT #3", GREEN,
        label_y=3.84, fontsize=8.2)
step(3.48, 8)
message(6.45, 3.75, 3.42, "PING", BLUE)
message(3.75, 6.45, 3.08, "PONG #4", GREEN)
step(2.72, 9)
message(6.45, 3.75, 2.66, "QUIT", BLUE)
message(3.75, 1.05, 2.32, "LEAVE #5", GREEN)
step(1.96, 10)
message(1.05, 3.75, 1.90, "TCP FIN / ACK", GRAY,
        linestyle=(0, (5, 3)))



ax.text(7.68, 0.72, "时间 ↓", ha="right", va="bottom", fontsize=8.5,
        color=GRAY)

fig.savefig(OUT, dpi=200, bbox_inches="tight", facecolor="white",
            pad_inches=0.12)
print(OUT)
