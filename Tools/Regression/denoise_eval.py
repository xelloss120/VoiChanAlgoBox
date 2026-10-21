# デノイズの評価: 声 + ノイズを通して、ノイズの下がり方と声の削れ方を測る
#   python denoise_eval.py denoise_method=3 denoise_fft_rule=1 ...   （設定を並べる。なければ初期値）
#   --synth で合成の母音（初期は音声合成の声）、--kind pink|white|fan でノイズの種類
import os
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import stft

import signals as sg

BASE = ['cut_on=0', 'notch_on=0', 'gate_on=0', 'voice_on=0', 'breath_on=0', 'eq_on=0', 'deess_on=0', 'comp_on=0', 'output_on=0', 'denoise_on=1']


def db(p):
    return 10 * np.log10(p + 1e-20)


def run(x, sets, tag):
    os.makedirs(os.path.join(sg.WORK, 'eval'), exist_ok=True)
    fi = os.path.join(sg.WORK, 'eval', f'in_{tag}.wav')
    fo = os.path.join(sg.WORK, 'eval', f'out_{tag}.wav')
    wavfile.write(fi, sg.SR, x.astype(np.float32))
    sg.render(fi, fo, BASE + sets)
    return wavfile.read(fo)[1].astype(float)


def evaluate(sets, label='', kind='pink', noise_db=-50, synth=False, latency=0):
    """ノイズだけ / 声の合間 / 声の区間 / 声が勝っている区画 / ノイズが勝っている区画 / 語頭 20ms の下がり（dB）を出す"""
    rng = np.random.default_rng(1)
    clean, mask = sg.synth_speech(rng, 10) if synth else sg.tts_speech()
    nz = sg.noise(rng, kind, len(clean), noise_db)
    y = run(clean + nz, sets, 'mix')
    yn = run(nz, sets, 'noise')
    y = np.concatenate([y[latency:], np.zeros(latency)])
    yn = np.concatenate([yn[latency:], np.zeros(latency)])

    tail = slice(int(4 * sg.SR), None)
    noise_only = db(np.mean(nz[tail] ** 2)) - db(np.mean(yn[tail] ** 2))
    gaps = ~mask
    gaps[:int(3 * sg.SR)] = False
    between = db(np.mean(nz[gaps] ** 2)) - db(np.mean(y[gaps] ** 2))
    speech = mask.copy()
    speech[:int(3 * sg.SR)] = False
    speech_loss = db(np.mean((clean + nz)[speech] ** 2)) - db(np.mean(y[speech] ** 2))

    # 周波数ごと: 声がノイズより 6dB 以上大きい区画と、その逆の区画の下がり（位相のずれは数えない）
    C, N, X, Y = [np.abs(stft(a, sg.SR, nperseg=1024)[2][:, 6:]) ** 2 for a in (clean, nz, clean + nz, y)]
    speech_bins = db(np.sum(X[C > N * 4])) - db(np.sum(Y[C > N * 4]))
    noise_bins = db(np.sum(X[N > C * 4])) - db(np.sum(Y[N > C * 4]))

    onset = np.zeros(len(clean), bool)
    for st in np.flatnonzero(np.diff(mask.astype(int)) == 1) + 1:
        if st > 3 * sg.SR:
            onset[st:st + int(0.02 * sg.SR)] = True
    onset_loss = db(np.mean((clean + nz)[onset] ** 2)) - db(np.mean(y[onset] ** 2))
    print(f'{label:20s} ノイズだけ {noise_only:5.1f}  合間 {between:5.1f}  声の区間 {speech_loss:5.2f}  '
          f'声の区画 {speech_bins:5.2f}  ノイズの区画 {noise_bins:5.1f}  語頭 {onset_loss:5.2f}')


if __name__ == '__main__':
    args = sys.argv[1:]
    kind = 'pink'
    if '--kind' in args:
        kind = args[args.index('--kind') + 1]
        del args[args.index('--kind'):args.index('--kind') + 2]
    synth = '--synth' in args
    sets = [a for a in args if '=' in a]
    evaluate(sets, ' '.join(sets) or '初期値', kind=kind, synth=synth)
