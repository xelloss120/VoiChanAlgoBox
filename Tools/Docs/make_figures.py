# 解説文書（docs/guide）の図を作る。
# 必要なもの: Python 3、numpy、scipy、matplotlib、ビルドした vcab_render（Release）、
# デノイズの図には Tools/Regression/make_tts.ps1 で作るテスト用の声（build/regression/tts.wav）。
# 使い方: python make_figures.py            （全部）
#         python make_figures.py basics voice  （名前の一部が合う図だけ）
import os
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import lfilter, freqz, butter, stft

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'Regression'))
from signals import SR, REPO, render, resonator, noise, tts_speech  # noqa: E402

import matplotlib  # noqa: E402
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

plt.rcParams['font.family'] = ['Yu Gothic', 'Meiryo', 'MS Gothic', 'sans-serif']
plt.rcParams['font.size'] = 11
plt.rcParams['axes.grid'] = True
plt.rcParams['grid.alpha'] = 0.3
plt.rcParams['figure.facecolor'] = 'white'

OUT = os.path.join(REPO, 'docs', 'guide', 'images')
WORK = os.path.join(REPO, 'build', 'docs')
os.makedirs(OUT, exist_ok=True)
os.makedirs(WORK, exist_ok=True)

C_IN, C_OUT, C_ENV, C_ACC = '#2f6db5', '#e07b00', '#c0392b', '#2e9b57'
OFF = ['cut_on=0', 'notch_on=0', 'repair_on=0', 'gate_on=0', 'breath_on=0', 'eq_on=0', 'deess_on=0', 'comp_on=0', 'output_on=0']
VOWELS = {'あ': (730, 1090, 2440), 'い': (270, 2290, 3010), 'う': (300, 870, 2240), 'え': (530, 1840, 2480), 'お': (570, 840, 2410)}

ALGORITHMS = ['ディレイライン式', 'WSOLA 式', 'TD-PSOLA', 'フェーズボコーダ', '正弦波モデル', '周波数シフト', '帯域ごとの周波数シフト',
              'LPC＋ディレイライン', 'LPC＋PSOLA', 'ケプストラム＋ディレイライン', 'ケプストラム＋PSOLA', 'メルケプ＋ディレイライン', 'メルケプ＋PSOLA',
              'チャンネルボコーダ', 'LPC ボコーダ', 'ケプストラムボコーダ', 'メルケプボコーダ', 'WORLD']
DENOISE = ['帯域分割式', 'くし形フィルタ＋帯域分割', 'FFT', 'FFT＋FIR', 'FFT＋帯域分割', 'WebRTC NS', 'WebRTC NS＋帯域分割',
           'RNNoise', 'RNNoise＋帯域分割', 'DeepFilterNet', 'DeepFilterNet＋帯域分割']


def save(fig, name):
    from PIL import Image
    fig.tight_layout()
    path = os.path.join(OUT, name)
    fig.savefig(path, dpi=100)
    plt.close(fig)
    # リポジトリを軽くするため、色数を 256 に減らして保存し直す（スペクトログラムの見た目はほぼ変わらない）
    Image.open(path).convert('RGB').quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(path, optimize=True)
    print('  ', name)


def db(x, floor=-120):
    return np.maximum(20 * np.log10(np.abs(x) + 1e-12), floor)


def pulse_train(f0, n, sr=SR):
    """f0（定数か配列）のパルス列"""
    f0 = np.broadcast_to(np.asarray(f0, float), (n,))
    return np.diff(np.floor(np.cumsum(f0 / sr)), prepend=0).astype(float)


def vowel(src, name):
    f1, f2, f3 = VOWELS[name]
    return resonator(src, f1, 80) + 0.5 * resonator(src, f2, 100) + 0.25 * resonator(src, f3, 150)


def vowel_response(name, f):
    """vowel() のフィルタの大きさ（f は Hz の配列）"""
    f1, f2, f3 = VOWELS[name]
    h = np.zeros(len(f), complex)
    for fc, bw, g in ((f1, 80, 1), (f2, 100, 0.5), (f3, 150, 0.25)):
        r = np.exp(-np.pi * bw / SR)
        th = 2 * np.pi * fc / SR
        _, hh = freqz([1 - r], [1, -2 * r * np.cos(th), r * r], worN=f, fs=SR)
        h += g * hh
    return np.abs(h)


def synth_vowels(order='あいうえお', seg=0.32, gap=0.06, f0=120.0, lead=0.1, with_s=True, seed=1):
    """男声に近い合成の母音の並び（声の高さがゆっくり動く。「さ」の子音つき）"""
    rng = np.random.default_rng(seed)
    out = [np.zeros(int(lead * SR))]
    for k, v in enumerate(order):
        L = int(seg * SR)
        if with_s and k == 2:
            fl = int(0.09 * SR)
            out.append(lfilter(*butter(4, 4000 / (SR / 2), 'high'), rng.standard_normal(fl)) * np.hanning(fl) * 0.05)
        t = np.arange(L) / SR
        f = f0 * (1 + 0.12 * np.sin(2 * np.pi * 1.3 * t + k)) * (1 - 0.05 * k / len(order))
        s = vowel(pulse_train(f, L) + 0.02 * rng.standard_normal(L), v)
        s *= np.minimum(1, np.minimum(np.arange(L), L - np.arange(L)) / (0.02 * SR))
        out += [s, np.zeros(int(gap * SR))]
    x = np.concatenate(out + [np.zeros(int(0.15 * SR))])
    return x * 0.25 / np.max(np.abs(x))


def write(path, x):
    wavfile.write(path, SR, (np.clip(x, -1, 1) * 32767).astype(np.int16))


def read(path):
    _, y = wavfile.read(path)
    return y.astype(float) / 32768.0 if y.dtype == np.int16 else y.astype(float)


def spectrogram(ax, x, title='', fmax=5000, nfft=2048, vmin=-90, log=False):
    f, t, Z = stft(x, SR, nperseg=nfft, noverlap=nfft * 7 // 8)
    S = db(Z)
    S -= S.max()
    if log:
        keep = f >= 60
        ax.pcolormesh(t, f[keep], S[keep], shading='auto', cmap='magma', vmin=vmin, vmax=0, rasterized=True)
        ax.set_yscale('log')
        ax.set_ylim(60, fmax)
    else:
        keep = f <= fmax
        ax.pcolormesh(t, f[keep], S[keep], shading='auto', cmap='magma', vmin=vmin, vmax=0, rasterized=True)
    ax.set_title(title, fontsize=11)
    ax.grid(False)
    ax.set_xlabel('時間 (秒)')
    ax.set_ylabel('周波数 (Hz)')


def spectrum(x, nfft=8192):
    w = np.hanning(len(x))
    X = np.fft.rfft(x * w, nfft)
    return np.fft.rfftfreq(nfft, 1 / SR), db(X / np.sum(w) * 2)


# ---------------------------------------------------------------- 基礎編

def fig_basics():
    print('基礎編')
    # サンプリング
    fig, ax = plt.subplots(figsize=(8, 3))
    t = np.linspace(0, 2, 2000)
    ax.plot(t, np.sin(2 * np.pi * t), color='#999', lw=1.5, label='元の音（なめらかな波）')
    ts = np.arange(0, 2.0001, 1 / 12)
    ax.stem(ts, np.sin(2 * np.pi * ts), linefmt=C_IN, markerfmt='o', basefmt=' ', label='サンプル（一定の間隔で測った値）')
    ax.set_xlabel('時間（1 周期 = 1）')
    ax.set_ylabel('音圧')
    ax.set_ylim(-1.2, 1.2)
    ax.legend(loc='lower center', bbox_to_anchor=(0.5, 1.0), ncol=2, fontsize=10, frameon=False)
    save(fig, 'b_sampling.png')

    # 波形とスペクトル
    L = int(0.5 * SR)
    a = vowel(pulse_train(120, L), 'あ')
    a /= np.max(np.abs(a))
    fig, axes = plt.subplots(1, 2, figsize=(10, 3.4))
    seg = a[SR // 4: SR // 4 + int(0.03 * SR)]
    axes[0].plot(np.arange(len(seg)) / SR * 1000, seg, color=C_IN)
    axes[0].set_xlabel('時間 (ms)')
    axes[0].set_title('波形（合成の「あ」、声の高さ 120Hz）')
    axes[0].annotate('', xy=(1000 / 120 + 2.2, 0.95), xytext=(2.2, 0.95), arrowprops=dict(arrowstyle='<->', color=C_ENV))
    axes[0].text(2.2 + 500 / 120, 1.02, '1 周期 = 8.3ms', color=C_ENV, ha='center')
    axes[0].set_ylim(-1.1, 1.25)
    f, S = spectrum(a[SR // 8: SR // 8 + 8192])
    axes[1].plot(f, S - S.max(), color=C_IN, lw=1)
    env = db(vowel_response('あ', f))
    axes[1].plot(f, env - env.max() + 2, color=C_ENV, lw=2, label='包絡（なだらかな形）')
    for k, fc in enumerate(VOWELS['あ']):
        axes[1].annotate(f'F{k + 1}', xy=(fc, 4), ha='center', color=C_ENV, fontweight='bold')
    axes[1].set_xlim(0, 4000)
    axes[1].set_ylim(-70, 10)
    axes[1].set_xlabel('周波数 (Hz)')
    axes[1].set_ylabel('大きさ (dB)')
    axes[1].set_title('スペクトル: 120Hz おきの倍音と、山（フォルマント）')
    axes[1].legend(loc='lower left', fontsize=9)
    save(fig, 'b_waveform_spectrum.png')

    # ソース・フィルタ
    f = np.linspace(1, 4000, 4000)
    harm = np.arange(120, 4000, 120)
    src_level = -6 * np.log2(harm / 120)          # 声帯の音: 高い倍音ほど小さい
    flt = db(vowel_response('あ', f))
    flt -= flt.max()
    flt_at = db(vowel_response('あ', harm)) - db(vowel_response('あ', f)).max()
    fig, axes = plt.subplots(1, 3, figsize=(12, 3.3), sharey=True)
    axes[0].vlines(harm, -80, src_level, color=C_IN)
    axes[0].set_title('声帯の音（ソース）\n声の高さの整数倍の倍音')
    axes[1].plot(f, flt, color=C_ENV, lw=2)
    axes[1].set_title('口と喉のフィルタ（フィルタ）\n山の位置 = フォルマント')
    axes[2].vlines(harm, -80, src_level + flt_at, color=C_IN)
    axes[2].plot(f, flt, color=C_ENV, lw=1, alpha=0.5)
    axes[2].set_title('声 = ソース × フィルタ\n倍音の大きさがフィルタの形をなぞる')
    for ax in axes:
        ax.set_xlim(0, 4000)
        ax.set_ylim(-70, 5)
        ax.set_xlabel('周波数 (Hz)')
    axes[0].set_ylabel('大きさ (dB)')
    save(fig, 'b_source_filter.png')

    # ピッチとフォルマント
    fig, axes = plt.subplots(1, 3, figsize=(12, 3.4), sharey=True)
    ref = db(vowel_response('あ', f)).max()
    for ax, (p, fm, title) in zip(axes, [(1, 1, '元の声（男声）\nピッチ 0、フォルマント 0'),
                                         (2, 1, 'ピッチだけ +12 半音\n倍音の間隔が 2 倍、山はそのまま'),
                                         (2, 2 ** 0.5, 'ピッチ +12、フォルマント +6\n山も 1.41 倍の位置へ')]):
        h = np.arange(120 * p, 4000, 120 * p)
        env = db(vowel_response('あ', f / fm)) - ref
        lv = db(vowel_response('あ', h / fm)) - ref - 6 * np.log2(h / 120 / p)
        ax.vlines(h, -80, lv, color=C_IN)
        ax.plot(f, env, color=C_ENV, lw=1.5)
        ax.set_title(title)
        ax.set_xlim(0, 4000)
        ax.set_ylim(-70, 5)
        ax.set_xlabel('周波数 (Hz)')
    axes[0].set_ylabel('大きさ (dB)')
    save(fig, 'b_pitch_formant.png')

    # スペクトログラム
    x = synth_vowels()
    write(os.path.join(WORK, 'vowels.wav'), x)
    fig, ax = plt.subplots(figsize=(10, 3.6))
    spectrogram(ax, x, 'スペクトログラム（合成の「あいうえお」）: 横が時間、縦が周波数、明るいほど大きい')
    t0 = 0.1
    for k, v in enumerate('あいうえお'):
        if k == 2:
            t0 += 0.09
        ax.text(t0 + 0.16, 4600, v, color='white', fontsize=14, ha='center', fontweight='bold')
        t0 += 0.38
    ax.text(0.885, 300, 'さ行の\n子音', color='white', fontsize=10, ha='center')
    save(fig, 'b_spectrogram.png')

    # 窓と分解能
    fig, axes = plt.subplots(1, 3, figsize=(13, 3.4))
    n = 2048
    t = np.arange(4 * n) / SR * 1000
    sig = x[int(0.5 * SR): int(0.5 * SR) + 4 * n]
    axes[0].plot(t, sig / np.max(np.abs(sig)), color='#999', lw=0.8)
    for k in range(5):
        st = k * n // 2
        w = np.zeros(4 * n)
        w[st:st + n] = np.hanning(n)
        axes[0].plot(t, w, lw=1.5)
    axes[0].set_title('窓: 少しずつずらしながら切り出す')
    axes[0].set_xlabel('時間 (ms)')
    spectrogram(axes[1], x, '窓 256（5ms）: 時間に細かく、周波数に粗い', nfft=256)
    spectrogram(axes[2], x, '窓 4096（85ms）: 周波数に細かく、時間に粗い', nfft=4096)
    save(fig, 'b_window_resolution.png')

    # 包絡の求め方
    seg = vowel(pulse_train(140, 8192), 'え')[4096:]
    w = np.hanning(len(seg))
    X = np.fft.rfft(seg * w, 4096)
    fr = np.fft.rfftfreq(4096, 1 / SR)
    S = db(X)
    # ケプストラム（2.5ms まで残す）
    c = np.fft.irfft(np.log(np.abs(X) + 1e-9))
    keep = int(0.0025 * SR)
    cl = c.copy()
    cl[keep:-keep] = 0
    cep = 20 / np.log(10) * np.real(np.fft.rfft(cl))
    # True Envelope（ケプストラムを繰り返して山に沿わせる）
    logA = np.log(np.abs(X) + 1e-9)
    te = logA.copy()
    for _ in range(30):
        cc = np.fft.irfft(te)
        cc[keep:-keep] = 0
        sm = np.real(np.fft.rfft(cc))
        te = np.maximum(logA, sm)
    te = 20 / np.log(10) * sm
    # LPC（12kHz に落として 14 次）
    from scipy.linalg import solve_toeplitz
    from scipy.signal import decimate
    xs = decimate(seg, 4)
    xs = xs * np.hanning(len(xs))
    r = np.correlate(xs, xs, 'full')[len(xs) - 1:len(xs) + 15]
    a = solve_toeplitz(r[:14], r[1:15])
    _, hl = freqz([1], np.concatenate([[1], -a]), worN=fr, fs=SR / 4)
    lpc = db(hl)
    fig, ax = plt.subplots(figsize=(9, 3.8))
    top = S[fr < 5000].max()
    ax.plot(fr, S - top, color='#aaa', lw=1, label='スペクトル（倍音の細かい山）')
    for y, col, lab in ((lpc, C_ENV, 'LPC 包絡'), (cep, C_IN, 'ケプストラム（2.5ms まで残す）'), (te, C_ACC, 'True Envelope')):
        ax.plot(fr, y - np.max(y[(fr > 100) & (fr < 5000)]) + 0, color=col, lw=2, label=lab)
    ax.set_xlim(0, 5000)
    ax.set_ylim(-60, 8)
    ax.set_xlabel('周波数 (Hz)')
    ax.set_ylabel('大きさ (dB)')
    ax.set_title('包絡（声色の形）の求め方の違い: 合成の「え」、声の高さ 140Hz')
    ax.legend(fontsize=9, loc='upper right')
    save(fig, 'b_envelopes.png')

    # ケプストラム
    fig, ax = plt.subplots(figsize=(9, 3.2))
    q = np.arange(len(c)) / SR * 1000
    ax.plot(q[:int(0.02 * SR)], c[:int(0.02 * SR)], color=C_IN)
    ax.axvspan(0, 2.5, color=C_ENV, alpha=0.15)
    ax.text(1.25, 0.55 * c[1:int(0.02 * SR)].max(), '声色\n（低いケフレンシー）', color=C_ENV, ha='center', fontsize=9)
    ax.annotate('声の高さの山\n（1/140Hz = 7.1ms）', xy=(1000 / 140, c[int(SR / 140)]), xytext=(11, 0.6 * c[1:int(0.02 * SR)].max()),
                arrowprops=dict(arrowstyle='->'), ha='left')
    ax.set_ylim(-0.4 * c[1:int(0.02 * SR)].max(), 1.1 * c[1:int(0.02 * SR)].max())
    ax.set_xlabel('ケフレンシー (ms)')
    ax.set_title('ケプストラム: 対数スペクトルをもう一度フーリエ変換すると、声色と声の高さが分かれる')
    save(fig, 'b_cepstrum.png')

    # メルの曲げ
    fig, ax = plt.subplots(figsize=(6, 4))
    w = np.linspace(0, np.pi, 500)
    for alpha, col in ((0.0, '#999'), (0.55, C_IN)):
        ww = w + 2 * np.arctan(alpha * np.sin(w) / (1 - alpha * np.cos(w)))
        ax.plot(w / np.pi * SR / 2 / 1000, ww / np.pi, color=col, lw=2, label=f'α = {alpha}')
    mel = 2595 * np.log10(1 + w / np.pi * SR / 2 / 700)
    ax.plot(w / np.pi * SR / 2 / 1000, mel / mel.max(), '--', color=C_ENV, label='メル尺度')
    ax.set_xlabel('元の周波数 (kHz)')
    ax.set_ylabel('曲げた後の軸（0〜1）')
    ax.set_title('メルケプストラムの周波数の曲げ（48kHz）')
    ax.legend()
    save(fig, 'b_mel_warp.png')

    # ピッチ検出（自己相関と YIN）
    W = int(0.02 * SR)
    lags = np.arange(1, int(0.025 * SR))
    seg = x[int(0.15 * SR): int(0.15 * SR) + W + len(lags) + 1]
    ac = np.array([np.sum(seg[:W] * seg[k:k + W]) for k in lags])
    d = np.array([np.sum((seg[:W] - seg[k:k + W]) ** 2) for k in lags])
    cmnd = d * lags / np.cumsum(d)
    fig, axes = plt.subplots(1, 2, figsize=(11, 3.3))
    axes[0].plot(lags / SR * 1000, ac / ac.max(), color=C_IN)
    axes[0].set_title('自己相関: ずらして重ねた一致度。山の位置が周期')
    axes[0].set_xlabel('ずらす量 (ms)')
    k = np.argmax(ac[int(SR / 400):]) + int(SR / 400)
    axes[0].axvline(lags[k] / SR * 1000, color=C_ENV, ls='--')
    axes[0].axvline(2 * lags[k] / SR * 1000, color=C_ENV, ls=':', alpha=0.6)
    axes[0].text(2 * lags[k] / SR * 1000, 0.8, ' 2 周期\n（オクターブの\n　間違いのもと）', color=C_ENV, fontsize=9)
    axes[1].plot(lags / SR * 1000, cmnd, color=C_IN)
    axes[1].axhline(0.2, color='#999', ls='--')
    axes[1].text(15, 0.24, 'しきい値', color='#666')
    axes[1].set_title('YIN: 差の 2 乗を正規化。最初に谷へ入る所が周期')
    axes[1].set_xlabel('ずらす量 (ms)')
    axes[1].set_ylim(0, 1.6)
    save(fig, 'b_pitch_detection.png')


# ---------------------------------------------------------------- ボイチェン編

def fig_voice():
    print('ボイチェン編')
    x = synth_vowels()
    src = os.path.join(WORK, 'vowels.wav')
    write(src, x)
    outs = []
    for i, name in enumerate(ALGORITHMS):
        out = os.path.join(WORK, f'voice_{i}.wav')
        render(src, out, OFF + ['denoise_on=0', 'voice_on=1', f'algorithm={i}'])
        outs.append(read(out))
    # 一覧
    fig, axes = plt.subplots(5, 4, figsize=(15, 15))
    axes = axes.ravel()
    spectrogram(axes[0], x, '入力（合成の男声）')
    for i, y in enumerate(outs):
        spectrogram(axes[i + 1], y, ALGORITHMS[i])
    for ax in axes[len(outs) + 1:]:
        ax.axis('off')
    for ax in axes:
        ax.set_xlabel('')
        ax.set_ylabel('')
    fig.suptitle('各方式の出力（ピッチ +12、フォルマント +6。縦軸 0〜5kHz）', fontsize=14)
    save(fig, 'v_gallery.png')
    # 方式ごと（入力と出力を並べる）
    for i, y in enumerate(outs):
        fig, axes = plt.subplots(1, 2, figsize=(11, 3.2), sharey=True)
        spectrogram(axes[0], x, '入力')
        spectrogram(axes[1], y, f'出力: {ALGORITHMS[i]}（ピッチ +12、フォルマント +6）')
        axes[1].set_ylabel('')
        save(fig, f'v_out_{i:02d}.png')

    # ディレイライン
    fig, axes = plt.subplots(2, 1, figsize=(9, 4.6), sharex=True)
    t = np.linspace(0, 0.09, 2000)
    win = 0.03
    ratio = 2.0
    for k, col in ((0, C_IN), (0.5, C_OUT)):
        ph = (t / win * (ratio - 1) + k) % 1
        axes[0].plot(t * 1000, (1 - ph) * win * 1000, color=col, lw=2, label=f'読み出し {1 if k == 0 else 2}')
        axes[1].plot(t * 1000, np.sin(np.pi * ph) ** 2, color=col, lw=2)
    axes[0].set_ylabel('遅れ (ms)')
    axes[0].set_title('ディレイライン式（ピッチ 2 倍）: 2 倍の速さで読むので遅れが減り、0 になったら戻る')
    axes[0].legend(loc='upper right', fontsize=9)
    axes[1].set_ylabel('混ぜる重み')
    axes[1].set_xlabel('時間 (ms)')
    axes[1].set_title('戻る瞬間が聞こえないように、2 本を交互に混ぜる（クロスフェード）')
    save(fig, 'v_delay_line.png')

    # WSOLA: そろえずに重ねると打ち消す
    n = int(0.02 * SR)
    t = np.arange(n) / SR
    s = np.sin(2 * np.pi * 150 * t) + 0.5 * np.sin(2 * np.pi * 300 * t + 1)
    fade = np.linspace(1, 0, n)
    bad = s * fade + np.sin(2 * np.pi * 150 * t + np.pi) * (1 - fade) + 0.5 * np.sin(2 * np.pi * 300 * t + 1 + 2 * np.pi) * (1 - fade)
    good = s * fade + s * (1 - fade)
    fig, axes = plt.subplots(1, 2, figsize=(11, 3), sharey=True)
    axes[0].plot(t * 1000, bad, color=C_ENV)
    axes[0].set_title('そのままつなぐ（ディレイライン式）\n波の山と谷が重なると打ち消し合う（うなり）')
    axes[1].plot(t * 1000, good, color=C_ACC)
    axes[1].set_title('似た位置を探してつなぐ（WSOLA 式）\n波の形がそろうので、なめらかにつながる')
    for ax in axes:
        ax.set_xlabel('つなぎ目の 20ms (ms)')
    save(fig, 'v_wsola.png')

    # TD-PSOLA
    T = int(SR / 125)
    L = 6 * T
    base = vowel(pulse_train(125, L + 4 * T), 'あ')[2 * T:2 * T + L]
    base /= np.max(np.abs(base))
    marks = np.argmax(base[:T]) + np.arange(0, L - T // 2, T)
    marks = marks[(marks > T) & (marks < L - T)]
    fig, axes = plt.subplots(3, 1, figsize=(10, 6.4), sharex=True)
    tt = np.arange(L) / SR * 1000
    axes[0].plot(tt, base, color=C_IN)
    axes[0].vlines(marks / SR * 1000, -1, 1, color=C_ENV, ls='--')
    axes[0].set_title('1. 声の 1 周期ごとに印を付ける（ピッチ検出の周期を使う）')
    for m in marks:
        w = np.hanning(2 * T)
        g = base[m - T:m + T] * w
        axes[1].plot(tt[m - T:m + T], g, lw=1.5)
    axes[1].set_title('2. 印を中心に 2 周期分を窓で切り出す（断片）')
    newT = T // 2
    out = np.zeros(L)
    cols = plt.rcParams['axes.prop_cycle'].by_key()['color']
    for k, pos in enumerate(range(marks[0], L - T, newT)):
        m = marks[min(int((pos - marks[0]) / T + 0.5), len(marks) - 1)]
        g = base[m - T:m + T] * np.hanning(2 * T)
        out[pos - T:pos + T] += g
        axes[2].vlines(pos / SR * 1000, -1.5, 1.5, color='#bbb', lw=0.8)
    axes[2].plot(tt, out, color=C_OUT)
    axes[2].set_title('3. 間隔を半分にして並べ直す（ピッチ 2 倍）。断片の中の形はそのまま = フォルマントは動かない')
    axes[2].set_xlabel('時間 (ms)')
    save(fig, 'v_psola.png')

    # フェーズボコーダ / 周波数シフト
    f = np.linspace(1, 4000, 4000)
    ref = db(vowel_response('あ', f)).max()
    fig, axes = plt.subplots(1, 3, figsize=(13, 3.4), sharey=True)
    h0 = np.arange(120, 4000, 120)
    lv0 = db(vowel_response('あ', h0)) - ref
    for ax, (h, lv, title) in zip(axes, [
            (h0, lv0, '元の声: 120Hz の整数倍'),
            (2 * h0, db(vowel_response('あ', 2 * h0)) - ref, 'ピッチシフト（×2、フォルマントは保つ）\n240Hz の整数倍。倍音の並びは保たれる'),
            (h0 + 120, lv0, '周波数シフト（＋120Hz）: 240, 360, 480…\n間隔は 120Hz のまま = 整数倍でない')]):
        ax.vlines(h[h < 4000], -80, lv[h < 4000], color=C_IN)
        ax.set_title(title)
        ax.set_xlim(0, 2000)
        ax.set_ylim(-60, 5)
        ax.set_xlabel('周波数 (Hz)')
    axes[0].set_ylabel('大きさ (dB)')
    save(fig, 'v_shift_compare.png')

    # LPC の分解
    from scipy.linalg import solve_toeplitz
    from scipy.signal import decimate
    sr4 = SR // 4                                  # 12kHz に落として 14 次（図を見やすくするため）
    xa = decimate(vowel(pulse_train(120, int(0.2 * SR)) + 1e-4 * np.random.default_rng(1).standard_normal(int(0.2 * SR)), 'あ'), 4)
    w = np.hanning(len(xa))
    r = np.correlate(xa * w, xa * w, 'full')[len(xa) - 1:len(xa) + 15]
    a = solve_toeplitz(r[:14], r[1:15])
    A = np.concatenate([[1], -a])
    res = lfilter(A, [1], xa)
    view = slice(int(0.1 * sr4), int(0.16 * sr4))
    fig, axes = plt.subplots(2, 2, figsize=(11, 5.4))
    tt = np.arange(view.stop - view.start) / sr4 * 1000
    axes[0, 0].plot(tt, xa[view] / np.max(np.abs(xa[view])), color=C_IN)
    axes[0, 0].set_title('声の波形')
    axes[1, 0].plot(tt, res[view] / np.max(np.abs(res[view])), color=C_OUT)
    axes[1, 0].set_title('LPC で予測しきれなかった残り（残差）\n= ほぼ声帯のパルス')
    axes[1, 0].set_xlabel('時間 (ms)')
    nf = 4096
    fr = np.fft.rfftfreq(nf, 1 / sr4)
    S = db(np.fft.rfft(xa * w, nf))
    R = db(np.fft.rfft(res * w, nf))
    _, hl = freqz([1], A, worN=fr, fs=sr4)
    axes[0, 1].plot(fr, S - S.max(), color='#aaa')
    axes[0, 1].plot(fr, db(hl) - db(hl).max(), color=C_ENV, lw=2, label='LPC の合成フィルタ 1/A(z)')
    axes[0, 1].legend(fontsize=9)
    axes[0, 1].set_title('声のスペクトル')
    axes[1, 1].plot(fr, R - R.max(), color=C_OUT)
    axes[1, 1].set_title('残差のスペクトル: 山がなく平ら（倍音だけ）')
    for ax in axes[:, 1]:
        ax.set_xlim(0, 5000)
        ax.set_ylim(-70, 5)
    axes[1, 1].set_xlabel('周波数 (Hz)')
    save(fig, 'v_lpc_residual.png')

    # チャンネルボコーダの帯域
    fig, ax = plt.subplots(figsize=(9, 3.2))
    edges = np.geomspace(100, 10000, 25)
    fr = np.geomspace(50, 16000, 2000)
    for k in range(24):
        b, aa = butter(2, [edges[k] / (SR / 2), edges[k + 1] / (SR / 2)], 'band')
        _, h = freqz(b, aa, worN=fr, fs=SR)
        ax.plot(fr, db(h ** 2), lw=1)
    ax.set_xscale('log')
    ax.set_ylim(-40, 3)
    ax.set_xlabel('周波数 (Hz)')
    ax.set_ylabel('dB')
    ax.set_title('チャンネルボコーダの帯域（24 帯域、100Hz〜10kHz を対数で等分）')
    save(fig, 'v_channel_bands.png')

    # 声帯の波形
    def rosenberg(n, op=0.4, cl=0.16):
        t = np.arange(n) / n
        g = np.zeros(n)
        a1 = t < op
        g[a1] = 0.5 * (1 - np.cos(np.pi * t[a1] / op))
        a2 = (t >= op) & (t < op + cl)
        g[a2] = np.cos(np.pi / 2 * (t[a2] - op) / cl)
        return g

    def lf_like(n):
        # LF（Rd = 1.0 前後）の流れの微分に近い形: 開く間に上がり、閉じる瞬間に鋭く下がって、すぐ戻る
        t = np.arange(n) / n
        te, ta = 0.62, 0.025
        g = np.zeros(n)
        a1 = t < te
        g[a1] = np.exp(3 * t[a1] / te) * np.sin(np.pi * t[a1] / (te * 0.82))
        g[a1] /= -g[a1].min() if g[a1].min() < 0 else 1
        a2 = t >= te
        g[a2] = -np.exp(-(t[a2] - te) / ta) * abs(g[a1][-1] if g[a1][-1] < 0 else 1)
        return g - g.mean()

    n = 400
    pulse = np.zeros(n)
    pulse[n // 2] = 1
    shapes = [('パルス列', pulse), ('Rosenberg（声門の流れの微分）', np.diff(rosenberg(n), prepend=0)), ('LF（声門の流れの微分）', lf_like(n))]
    fig, axes = plt.subplots(2, 3, figsize=(12, 5))
    for k, (name, g) in enumerate(shapes):
        g = g / np.max(np.abs(g))
        axes[0, k].plot(np.arange(n) / n, g, color=C_IN)
        axes[0, k].set_title(name)
        axes[0, k].set_xlabel('1 周期')
        rep = np.tile(g, 40)
        Gf = np.abs(np.fft.rfft(rep))
        hidx = np.arange(1, 30) * 40
        axes[1, k].stem(np.arange(1, 30), db(Gf[hidx]) - db(Gf[hidx]).max(), basefmt=' ')
        axes[1, k].set_ylim(-50, 5)
        axes[1, k].set_xlabel('倍音の番号')
    axes[1, 0].set_ylabel('大きさ (dB)')
    save(fig, 'v_glottal.png')

    # フォルマントの動かし方
    fr = np.geomspace(100, 8000, 600)
    s = np.clip(np.log(fr / 150) / np.log(1000 / 150), 0, 1)
    s = 0.5 - 0.5 * np.cos(np.pi * s)
    marks = np.array([500, 1400, 2700])
    diffs = np.array([0, 4, 2])
    lf = np.log(fr)
    lm = np.log(marks)
    band = np.interp(lf, lm, diffs)
    for k in range(2):
        m = (lf > lm[k]) & (lf < lm[k + 1])
        u = (lf[m] - lm[k]) / (lm[k + 1] - lm[k])
        band[m] = diffs[k] + (diffs[k + 1] - diffs[k]) * (0.5 - 0.5 * np.cos(np.pi * u))
    fig, ax = plt.subplots(figsize=(9, 3.8))
    ax.plot(fr, np.full_like(fr, 6), color='#666', lw=2, label='一律（+6 半音）')
    ax.plot(fr, 6 * s, color=C_IN, lw=2, label='低い側を残す')
    ax.plot(fr, 6 + band, color=C_OUT, lw=2, label='帯域ごと（F1 0 / F2 +4 / F3 +2 の差）')
    ax.scatter(marks, 6 + diffs, color=C_OUT, zorder=5)
    for mk, nm in zip(marks, ('F1', 'F2', 'F3')):
        ax.text(mk, 6 + diffs[list(marks).index(mk)] + 0.5, nm, ha='center', color=C_OUT)
    ax.set_xscale('log')
    ax.set_xticks([100, 200, 500, 1000, 2000, 5000])
    ax.set_xticklabels(['100', '200', '500', '1k', '2k', '5k'])
    ax.set_xlabel('元の周波数 (Hz)')
    ax.set_ylabel('動く量（半音）')
    ax.set_ylim(-1, 12)
    ax.set_title('フォルマントの動かし方（つまみ +6 のとき）')
    ax.legend(fontsize=9, loc='upper left')
    save(fig, 'v_formant_warp.png')


# ---------------------------------------------------------------- デノイズ編

def fig_denoise():
    print('デノイズ編')
    speech, _ = tts_speech()
    speech = np.concatenate([np.zeros(SR), speech])   # 前の 3 秒はノイズだけ（自動の見積もりが落ち着くまで）。図はその後から
    x = speech + noise(np.random.default_rng(5), 'pink', len(speech), -42)
    src = os.path.join(WORK, 'noisy.wav')
    write(src, x)
    view = slice(int(2.8 * SR), int(6.8 * SR))
    outs = []
    for i, name in enumerate(DENOISE):
        out = os.path.join(WORK, f'denoise_{i}.wav')
        render(src, out, OFF + ['voice_on=0', 'denoise_on=1', f'denoise_method={i}'])
        outs.append(read(out))
    fig, axes = plt.subplots(4, 3, figsize=(15, 12))
    axes = axes.ravel()
    spectrogram(axes[0], x[view], '入力（音声合成の声＋ピンクノイズ）', fmax=8000, log=True)
    for i, y in enumerate(outs):
        spectrogram(axes[i + 1], y[view], DENOISE[i], fmax=8000, log=True)
    for ax in axes:
        ax.set_xlabel('')
        ax.set_ylabel('')
    fig.suptitle('各方式の出力（初期値: 削る量の上限 24dB。縦軸は対数 60Hz〜8kHz）', fontsize=14)
    save(fig, 'd_gallery.png')

    # ミュージカルノイズ（ノイズだけの区間）
    nz = noise(np.random.default_rng(9), 'pink', 4 * SR, -40)
    write(os.path.join(WORK, 'noise_only.wav'), nz)
    fig, axes = plt.subplots(1, 3, figsize=(14, 3.4), sharey=True)
    spectrogram(axes[0], nz[2 * SR:], 'ノイズだけの入力', fmax=8000, vmin=-70)
    for ax, (rule, title) in zip(axes[1:], ((0, 'スペクトル減算（ならし方なし）\nぽつぽつ残る = ミュージカルノイズ'),
                                            (1, 'ウィーナー＋判定指向型（ならし方なし）\n残りが均一'))):
        out = os.path.join(WORK, f'musical_{rule}.wav')
        render(os.path.join(WORK, 'noise_only.wav'), out,
               OFF + ['voice_on=0', 'denoise_on=1', 'denoise_method=2', f'denoise_fft_rule={rule}', 'denoise_smoothing=0', 'denoise_max_reduction=12'])
        y = read(out)[2 * SR:]
        f_, t_, Z = stft(y, SR, nperseg=2048, noverlap=2048 * 7 // 8)
        _, _, Z0 = stft(nz[2 * SR:], SR, nperseg=2048, noverlap=2048 * 7 // 8)
        S = db(Z) - db(Z0).max()
        keep = f_ <= 8000
        ax.pcolormesh(t_, f_[keep], S[keep], shading='auto', cmap='magma', vmin=-70, vmax=0, rasterized=True)
        ax.set_title(title, fontsize=11)
        ax.grid(False)
        ax.set_xlabel('時間 (秒)')
    save(fig, 'd_musical_noise.png')

    # ゲインの曲線
    snr = np.linspace(-15, 30, 400)          # 声（またはその帯域の音）とノイズの比（dB）
    floor = -24
    gate = np.where(snr > 6, 0.0, floor)
    exp_ = np.minimum(0, (snr - 6) * (3 - 1))
    xi = 10 ** (snr / 10)
    wien = 20 * np.log10(xi / (1 + xi))
    fig, ax = plt.subplots(figsize=(8, 4))
    for y, lab, col in ((gate, 'ゲート式（しきい値 6dB）', '#888'), (exp_, 'エキスパンダー式（しきい値 6dB、比率 3）', C_ACC),
                        (wien, 'ウィーナー（SNR 式）', C_IN)):
        ax.plot(snr, np.maximum(y, floor), lw=2, label=lab, color=col)
    ax.axhline(floor, color='#bbb', ls=':')
    ax.text(-14, floor + 1, '削る量の上限（-24dB）', color='#888')
    ax.set_xlabel('その帯域・区画の、ノイズに対する大きさ (dB)')
    ax.set_ylabel('残す量 (dB)')
    ax.set_title('下げる量の決め方: 「ノイズに対してどれだけ大きいか」から、残す量を決める')
    ax.legend(fontsize=9)
    save(fig, 'd_gain_rules.png')

    # ノイズの見積もり（最小値）
    rng = np.random.default_rng(2)
    T = np.arange(0, 8, 0.01)
    noise_lvl = -50 + 4 * (T > 5)
    p = noise_lvl + 3 * rng.standard_normal(len(T))
    talk = np.zeros(len(T), bool)
    for s0 in (0.8, 2.0, 3.1, 5.6, 6.7):
        talk |= (T > s0) & (T < s0 + 0.7)
    p = np.where(talk, p + 20 + 5 * rng.standard_normal(len(T)), p)
    sm = lfilter([0.2], [1, -0.8], p)
    mn = np.array([sm[max(0, i - 160):i + 1].min() for i in range(len(T))])
    fig, ax = plt.subplots(figsize=(9, 3.4))
    ax.plot(T, sm, color='#aaa', label='ある帯域の大きさ（ならしたもの）')
    ax.plot(T, mn + 4, color=C_ENV, lw=2, label='見積もり（直近 1.6 秒の最小値 × 補正）')
    ax.plot(T, noise_lvl, '--', color='#333', label='本当のノイズの大きさ')
    for s0 in (0.8, 2.0, 3.1, 5.6, 6.7):
        ax.axvspan(s0, s0 + 0.7, color=C_IN, alpha=0.08)
    ax.text(0.85, -22, '声', color=C_IN)
    ax.annotate('ノイズが上がると、少し遅れて追いつく', xy=(6.4, -47), xytext=(3.3, -62), arrowprops=dict(arrowstyle='->'))
    ax.set_ylim(-66, -18)
    ax.set_xlabel('時間 (秒)')
    ax.set_ylabel('dB')
    ax.set_title('自動（最小値）の見積もり: しゃべっていても、合間の一番静かなところを拾う')
    ax.legend(fontsize=8, loc='upper right')
    save(fig, 'd_noise_estimate.png')

    # 帯域分割の帯域（20 帯域と 32 帯域）
    def lp_diff_bands(edges, order):
        fr = np.geomspace(50, 20000, 2000)
        hs, prev = [], np.ones(len(fr), complex) * 0
        lps = []
        for e in edges:
            b, aa = butter(order, e / (SR / 2))
            _, h = freqz(b, aa, worN=fr, fs=SR)
            lps.append(h)
        bands = [lps[0]] + [lps[k] - lps[k - 1] for k in range(1, len(lps))] + [1 - lps[-1]]
        return fr, bands

    erb = lambda f: 21.4 * np.log10(1 + 0.00437 * f)  # noqa: E731
    inv = lambda e: (10 ** (e / 21.4) - 1) / 0.00437  # noqa: E731
    e32 = inv(np.linspace(erb(100), erb(16000), 32))
    e20 = np.concatenate([[150, 250, 350, 500], 500 * 2 ** (np.arange(1, 16) / 3)])
    fig, axes = plt.subplots(1, 2, figsize=(13, 3.4), sharey=True)
    for ax, (edges, order, title) in zip(axes, ((e20[e20 < 16500], 4, '帯域分割式（20 帯域、4 次の低域通過の差）'),
                                                (e32, 2, 'FFT＋帯域分割（32 帯域、ERB の等間隔）'))):
        fr, bands = lp_diff_bands(edges, order)
        for h in bands:
            ax.plot(fr, db(h, -60), lw=1)
        ax.plot(fr, db(sum(bands), -60), color='k', lw=2, ls='--', label='全部を足したもの（= 元の音）')
        ax.set_xscale('log')
        ax.set_ylim(-40, 3)
        ax.set_title(title)
        ax.set_xlabel('周波数 (Hz)')
        ax.legend(fontsize=8, loc='lower left')
    axes[0].set_ylabel('dB')
    save(fig, 'd_bands.png')

    # くし形フィルタ
    f0 = 120
    T = SR / f0
    fr = np.linspace(1, 2000, 4000)
    z = np.exp(-2j * np.pi * fr / SR)
    avg = sum(z ** (k * T) for k in range(1, 5)) / 4
    depth = 9
    a = 1 - 10 ** (-depth / 20)
    h = 1 - a * (1 - avg)
    fig, ax = plt.subplots(figsize=(9, 3))
    ax.plot(fr, db(h), color=C_IN, lw=1.5)
    ax.vlines(np.arange(f0, 2000, f0), -12, 1, color=C_ENV, ls=':', lw=1)
    ax.set_ylim(-12, 2)
    ax.set_xlabel('周波数 (Hz)')
    ax.set_ylabel('dB')
    ax.set_title('くし形フィルタ（声の高さ 120Hz、深さ 9dB）: 倍音（点線）はそのまま、間を下げる')
    save(fig, 'd_comb.png')


def main():
    want = sys.argv[1:]
    for name, fn in (('basics', fig_basics), ('voice', fig_voice), ('denoise', fig_denoise)):
        if not want or any(w in name for w in want):
            fn()


if __name__ == '__main__':
    main()
