# 回帰テストと評価で使う、入力の音の作り方と vcab_render の呼び出し
import os
import subprocess

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, lfilter

SR = 48000
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
WORK = os.path.join(REPO, 'build', 'regression')                      # 入力・基準・結果の置き場（git の外）
EXE = os.path.join(REPO, 'build', 'vcab_render_artefacts', 'Release', 'vcab_render.exe')
TTS = os.path.join(WORK, 'tts.wav')                                     # make_tts.ps1 で作る（Windows の音声合成）


def render(src, out, sets, extra_args=()):
    """vcab_render で src を処理して out に書く。sets は 'key=value' の並び"""
    args = [EXE, '--in', src, '--out', out, *extra_args]
    for s in sets:
        args += ['--set', s]
    return subprocess.run(args, check=True, capture_output=True, text=True, encoding='utf-8', errors='replace')


def resonator(x, f, bw):
    r = np.exp(-np.pi * bw / SR)
    th = 2 * np.pi * f / SR
    return lfilter([1 - r], [1, -2 * r * np.cos(th), r * r], x)


def noise(rng, kind, n, db):
    """白色 / ピンク / 低めのこもった雑音（fan）。RMS を db dBFS に合わせる"""
    w = rng.standard_normal(n)
    if kind == 'pink':
        w = lfilter([0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400], w)
    elif kind == 'fan':
        w = lfilter([1], [1, -0.97], w)
    return w * 10 ** (db / 20) / np.sqrt(np.mean(w ** 2))


def tts_speech():
    """音声合成の声（前に 2 秒、後に 0.5 秒の無音）と、声の区間（10ms の音量が最大から 45dB 以内）"""
    if not os.path.exists(TTS):
        raise FileNotFoundError(f'{TTS} がありません。make_tts.ps1 で作ってください')
    _, x = wavfile.read(TTS)
    x = x.astype(float) / 32768.0
    x = np.concatenate([np.zeros(2 * SR), x, np.zeros(SR // 2)])
    x *= 0.3 / np.max(np.abs(x))
    e = np.convolve(x ** 2, np.ones(480) / 480, 'same')
    return x, e > np.max(e) * 10 ** (-45 / 10)


def synth_speech(rng, dur):
    """合成の母音の並び（f0 約 120Hz、ときどきサ行のような子音）と、声の区間"""
    n = int(dur * SR)
    out = np.zeros(n)
    mask = np.zeros(n, bool)
    t = 2.0  # 最初の 2 秒はノイズだけ
    vowels = [(730, 1090, 2440), (270, 2290, 3010), (300, 870, 2240), (530, 1840, 2480), (570, 840, 2410)]
    k = 0
    while t < dur - 0.5:
        L = int(rng.uniform(0.15, 0.35) * SR)
        s = int(t * SR)
        f0 = 120 + 25 * np.sin(np.linspace(0, 3, L)) + rng.uniform(-10, 10)
        src = np.diff(np.floor(np.cumsum(f0 / SR)), prepend=0).astype(float) + 0.03 * rng.standard_normal(L)
        f1, f2, f3 = vowels[k % 5]
        k += 1
        v = resonator(src, f1, 80) + 0.5 * resonator(src, f2, 100) + 0.25 * resonator(src, f3, 150)
        v *= np.minimum(1, np.minimum(np.arange(L), L - np.arange(L)) / (0.02 * SR))
        if k % 3 == 0:
            fl = int(0.08 * SR)
            fr = lfilter(*butter(4, 4000 / (SR / 2), 'high'), rng.standard_normal(fl)) * np.hanning(fl) * 0.15
            if s - fl > 0:
                out[s - fl:s] += fr
                mask[s - fl:s] = True
        out[s:s + L] += v
        mask[s:s + L] = True
        t += L / SR + rng.uniform(0.08, 0.25)
    return out * 0.2 / np.max(np.abs(out)), mask


def with_transients(x):
    """リペアの試験用: -60dB のノイズ、クリック、パチパチ、吹かれ、音割れを入れる"""
    rng = np.random.default_rng(7)
    y = x.copy() + 10 ** (-60 / 20) * rng.standard_normal(len(x))
    for t in np.arange(2.3, len(x) / SR - 1, 0.37):           # クリック（1ms）
        i = int(t * SR)
        y[i:i + 48] += 0.2 * np.hanning(48) * np.sign(rng.standard_normal())
    for t in rng.uniform(2, len(x) / SR - 1, 60):             # パチパチ（1〜3 サンプル）
        i = int(t * SR)
        y[i:i + rng.integers(1, 4)] += rng.uniform(0.02, 0.12) * np.sign(rng.standard_normal())
    i = int(3.0 * SR)
    y[i:i + int(0.04 * SR)] += 0.3 * np.sin(2 * np.pi * 60 * np.arange(int(0.04 * SR)) / SR)  # 吹かれ
    y[int(4 * SR):int(4.5 * SR)] = np.clip(y[int(4 * SR):int(4.5 * SR)] * 5, -0.95, 0.95)        # 音割れ
    return np.clip(y, -1, 1)


def with_breaths(level_db, rng=None):
    """息継ぎの試験用: 言葉の合間（0.35 秒以上の無音）に、作った息継ぎを入れる。(元, 入れた音, 声の区間, 息継ぎの区間, 言葉の出だし)"""
    rng = rng or np.random.default_rng(3)

    def breath_sound(n):
        w = rng.standard_normal(n)
        y = lfilter(*butter(2, [400 / (SR / 2), 4500 / (SR / 2)], 'band'), w)
        y = y + 0.6 * resonator(w, 1200, 400) * 8 + 0.4 * resonator(w, 2600, 600) * 8
        y = y - 0.5 * lfilter(*butter(2, 4000 / (SR / 2), 'high'), y)
        env = np.ones(n)
        a1, r1 = int(0.08 * SR), int(0.10 * SR)
        env[:a1] = 0.5 - 0.5 * np.cos(np.pi * np.arange(a1) / a1)
        env[-r1:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(r1) / r1)
        return y * env / np.sqrt(np.mean(y ** 2))

    x, mask = tts_speech()
    x = x + 10 ** (-60 / 20) * rng.standard_normal(len(x))
    speech_rms = np.sqrt(np.mean(x[mask] ** 2))
    y = x.copy()
    breaths, onsets = [], []
    for o in np.flatnonzero(mask[1:] & ~mask[:-1]) + 1:
        prev = np.flatnonzero(mask[:o])
        prev_end = prev[-1] if len(prev) else 0
        gap = (o - prev_end) / SR
        if gap < 0.35 or o < SR:
            continue
        L = int(min(rng.uniform(0.25, 0.45), gap - 0.2) * SR)
        end = o - int(rng.uniform(0.06, 0.12) * SR)
        st = end - L
        if st <= prev_end + int(0.05 * SR):
            continue
        y[st:end] += breath_sound(L) * speech_rms * 10 ** (level_db / 20)
        breaths.append((st, end))
        onsets.append(o)
    return x, y, mask, breaths, onsets
