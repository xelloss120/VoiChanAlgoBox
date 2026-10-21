# 回帰テスト: 処理の音がビット単位で変わっていないかを確かめる
#   python regress.py make    今のビルドで基準を作る（build/regression/ref）
#   python regress.py check   今のビルドの音を基準と比べる（結果は build/regression/out）
#   python regress.py check 名前の一部 ...   名前にその文字列を含むものだけ
# 先に make_tts.ps1 で build/regression/tts.wav を作っておく。
import os
import sys

import numpy as np
from scipy.io import wavfile

import signals as sg

# ---- 入力 ----
def make_inputs():
    os.makedirs(os.path.join(sg.WORK, 'in'), exist_ok=True)
    x, _ = sg.tts_speech()
    inputs = {
        'clean': x,                                                             # 声だけ
        'noisy': x + sg.noise(np.random.default_rng(1), 'pink', len(x), -45),   # 声 + ピンクノイズ -45dBFS
        'repair': sg.with_transients(x),                                         # クリック、パチパチ、吹かれ、音割れ入り
        'breath': sg.with_breaths(-18)[1],                                       # 息継ぎ入り
    }
    paths = {}
    for name, sig in inputs.items():
        paths[name] = os.path.join(sg.WORK, 'in', name + '.wav')
        wavfile.write(paths[name], sg.SR, sig.astype(np.float32))
    return paths


# ---- 場合 ----（名前, 入力, 設定）
OFF = ['cut_on=0', 'notch_on=0', 'gate_on=0', 'breath_on=0', 'eq_on=0', 'deess_on=0', 'comp_on=0', 'output_on=0']
DN = OFF + ['voice_on=0', 'denoise_on=1']        # デノイズだけ
VC = OFF + ['denoise_on=0', 'voice_on=1']        # ボイチェンだけ
RP = OFF + ['denoise_on=0', 'voice_on=0', 'repair_on=1']  # リペアだけ

CASES = []
def case(name, inp, sets):
    CASES.append((name, inp, sets))

# デノイズ
for name, sets in [
    ('band_snr', ['denoise_method=0']),
    ('band_gate', ['denoise_method=0', 'denoise_rule=0']),
    ('band_exp', ['denoise_method=0', 'denoise_rule=1']),
    ('comb', ['denoise_method=1']),
    ('fft_sub', ['denoise_method=2', 'denoise_fft_rule=0']),
    ('fft_sub_cep', ['denoise_method=2', 'denoise_fft_rule=0', 'denoise_smoothing=3']),
    ('fft_wiener', ['denoise_method=2', 'denoise_fft_rule=1']),
    ('fft_wiener_1024_time', ['denoise_method=2', 'denoise_fft_rule=1', 'denoise_fft_size=2', 'denoise_smoothing=1']),
    ('fft_stsa_pres', ['denoise_method=2', 'denoise_fft_rule=2', 'denoise_presence=1']),
    ('fft_logmmse_freq', ['denoise_method=2', 'denoise_fft_rule=3', 'denoise_smoothing=2']),
    ('fft_hrnr', ['denoise_method=2', 'denoise_fft_rule=4']),
    ('fft_protect', ['denoise_method=2', 'denoise_fft_rule=5', 'denoise_fft_size=2']),
    ('lowlat', ['denoise_method=3', 'denoise_fft_rule=1']),
    ('lowlat_band', ['denoise_method=4', 'denoise_fft_rule=1']),
    ('rnnoise', ['denoise_method=7']),
    ('rnnoise_band', ['denoise_method=8']),
    ('webrtc', ['denoise_method=5']),
    ('webrtc_band', ['denoise_method=6']),
    ('dfn', ['denoise_method=9']),
    ('dfn_ll', ['denoise_method=9', 'denoise_dfn_model=1']),
    ('spp_mode', ['denoise_method=2', 'denoise_fft_rule=1', 'denoise_noise_mode=2']),
    ('ai_voice_mode', ['denoise_noise_mode=3']),
    ('ai_voice_band', ['denoise_method=0', 'denoise_noise_mode=3']),
    ('ai_voice_low', ['denoise_noise_mode=3', 'denoise_rnn_threshold=30']),
    ('level_mode', ['denoise_noise_mode=1']),
    ('level_band', ['denoise_method=0', 'denoise_noise_mode=1', 'denoise_level_threshold=10']),
    ('min_window_long', ['denoise_noise_mode=0', 'denoise_min_window=3.2']),
    ('min_window_mid', ['denoise_method=0', 'denoise_noise_mode=0', 'denoise_min_window=2.2']),
    ('spp_snr_low', ['denoise_noise_mode=2', 'denoise_spp_snr=8']),
    ('dereverb_band', ['denoise_method=0', 'denoise_dereverb_on=1']),
    ('dereverb_fft', ['denoise_method=2', 'denoise_fft_rule=1', 'denoise_dereverb_on=1']),
    ('gate_link', ['denoise_method=0', 'gate_on=1', 'gate_from_denoise=1']),
]:
    case(name, 'noisy', DN + sets)
case('full_default', 'noisy', [])  # すべて初期値

# ボイチェン（方式の番号は Choices.h の Algorithm の順番）
ALG = ['delayLine', 'wsola', 'tdPsola', 'phaseVocoder', 'sinusoidal', 'frequencyShift', 'bandShift', 'lpcDelayLine', 'lpcPsola',
       'cepstrumDelayLine', 'cepstrumPsola', 'melDelayLine', 'melPsola', 'channelVocoder', 'lpcVocoder', 'cepstrumVocoder', 'melVocoder', 'world']
def voice(name, *sets):
    """ALG の名前の方式で処理する場合。名前は「方式の名前_付け足し」"""
    algo = name.split('_')[0]
    case(name, 'clean', VC + [f'algorithm={ALG.index(algo)}', *sets])

for a in ALG:
    voice(a)
for a in ('channelVocoder', 'lpcVocoder', 'cepstrumVocoder'):
    voice(f'{a}_nomix', 'mixed_excitation=0')
voice('lpcVocoder_f0', 'formant=0')
voice('tdPsola_warp', 'formant_warp=1')
voice('melDelayLine_warp', 'formant_warp=1')
for a in ('channelVocoder', 'lpcVocoder', 'melVocoder'):
    for g in (1, 2):
        voice(f'{a}_g{g}', f'glottal_shape={g}')
voice('channelVocoder_g2_nomix', 'glottal_shape=2', 'mixed_excitation=0')
BAND = ['formant_warp=2', 'formant_f1=-3', 'formant_f2=4', 'formant_f3=2']
TRACK = ['formant_warp=3', 'formant_f1=-3', 'formant_f2=4', 'formant_f3=2']
for a in ('tdPsola', 'phaseVocoder', 'sinusoidal', 'lpcDelayLine', 'channelVocoder', 'bandShift', 'world', 'melDelayLine', 'melVocoder'):
    voice(f'{a}_band', *BAND)
    voice(f'{a}_track', *TRACK)
voice('tdPsola_band_f0', 'formant=0', *BAND)

# リペア
for name, sets in [
    ('r_none', []),
    ('r_declip', ['repair_declip_on=1', 'repair_clip_level=-1']),
    ('r_plosive', ['repair_plosive_on=1']),
    ('r_trans', ['repair_transient_on=1']),
    ('r_trans_lsar', ['repair_transient_on=1', 'repair_transient_mode=1']),
    ('r_click', ['repair_click_on=1']),
    ('r_click_lsar', ['repair_click_on=1', 'repair_click_mode=1']),
    ('r_crackle', ['repair_crackle_on=1']),
    ('r_all', ['repair_declip_on=1', 'repair_plosive_on=1', 'repair_transient_on=1', 'repair_click_on=1', 'repair_crackle_on=1']),
]:
    case(name, 'repair', RP + sets)
case('b_breath', 'breath', RP + ['repair_breath_on=1'])
case('b_breath_m6', 'breath', RP + ['repair_breath_on=1', 'repair_breath_margin=6', 'repair_breath_depth=20'])


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'check'
    filters = sys.argv[2:]
    inputs = make_inputs()
    target = os.path.join(sg.WORK, 'ref' if mode == 'make' else 'out')
    os.makedirs(target, exist_ok=True)
    bad = 0
    for name, inp, sets in CASES:
        if filters and not any(f in name for f in filters):
            continue
        out = os.path.join(target, name + '.wav')
        sg.render(inputs[inp], out, sets)
        if mode == 'check':
            _, a = wavfile.read(os.path.join(sg.WORK, 'ref', name + '.wav'))
            _, b = wavfile.read(out)
            d = np.max(np.abs(a.astype(float) - b.astype(float))) if a.shape == b.shape else float('inf')
            if d != 0:
                bad += 1
                print(f'{name:22s} 違う（最大の差 {d:.2e}）')
    print('作った' if mode == 'make' else f'ずれ {bad} 件')


if __name__ == '__main__':
    main()
