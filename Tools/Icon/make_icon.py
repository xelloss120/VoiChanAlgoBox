# アプリのアイコン（Resources/Icon.png と Resources/IconSmall.png）を描く。
# 図柄: 紺の角丸の土台に、枠付きの箱。箱の中の薄い横線（左が元の声、右が上へ移った声）を背景に、
# 元の声色の山（青）と、右へ動いた声色の山（黄）を重ねる。色はどれもアプリの画面の色。
# 必要なもの: Python 3、numpy、matplotlib、Pillow。使い方: python make_icon.py
import os

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
from PIL import Image

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(REPO, 'Resources')

BG = '#17202e'       # ウィンドウの背景
PANEL = '#273550'    # 枠の中
BEFORE = '#60c0ff'   # 声の高さのグラフの青（元の声色）
AFTER = '#ffd060'    # フォルマントのグラフの点の黄（動かした声色）
EDGE = '#4a90e0'     # つまみの塗りの青（箱の枠）


def envelope(x, shift):
    return (0.30 + 0.30 * np.exp(-((x - 0.36 * shift) / 0.06) ** 2) + 0.17 * np.exp(-((x - 0.55 * shift) / 0.055) ** 2)
            + 0.08 * np.exp(-((x - 0.72 * shift) / 0.045) ** 2))


def draw(path, small):
    """small: 16〜48px 用（背景の横線を省き、線を太くする）"""
    fig = plt.figure(figsize=(10.24, 10.24), dpi=100)
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis('off')
    fig.patch.set_alpha(0)
    ax.add_patch(FancyBboxPatch((0.04, 0.04), 0.92, 0.92, boxstyle='round,pad=0,rounding_size=0.2', fc=BG, ec=BG))
    inset = 0.14 if small else 0.18
    ax.add_patch(FancyBboxPatch((inset, inset), 1 - 2 * inset, 1 - 2 * inset, boxstyle='round,pad=0,rounding_size=0.08',
                                fc=PANEL, ec=EDGE, lw=30 if small else 14))
    if not small:
        for k in range(4):
            ax.plot([0.26, 0.48], [0.30 + k * 0.08] * 2, color=BEFORE, lw=26, solid_capstyle='round', alpha=0.45 * (1 - 0.15 * k))
            ax.plot([0.54, 0.74], [0.36 + k * 0.13] * 2, color=AFTER, lw=26, solid_capstyle='round', alpha=0.45 * (1 - 0.15 * k))
    x = np.linspace(0.22, 0.78, 500)
    ax.plot(x, envelope(x, 1.0), color=BEFORE, lw=34 if small else 18, solid_capstyle='round')
    ax.plot(x, envelope(x, 1.14), color=AFTER, lw=44 if small else 24, solid_capstyle='round')
    fig.savefig(path, dpi=100, transparent=True)
    plt.close(fig)
    print(path)


def main():
    os.makedirs(OUT, exist_ok=True)
    draw(os.path.join(OUT, 'Icon.png'), small=False)
    small = os.path.join(OUT, 'IconSmall.png')
    draw(small, small=True)
    Image.open(small).resize((256, 256), Image.LANCZOS).save(small, optimize=True)


if __name__ == '__main__':
    main()
