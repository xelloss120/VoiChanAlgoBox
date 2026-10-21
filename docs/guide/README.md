# アルゴリズム解説

VoiChanAlgoBox に入っている、ボイスチェンジャーとデノイズのアルゴリズムの解説です。仕組みを図で見て、実際に VoiChanAlgoBox で音を聞き比べながら読むことを想定しています。

- **基礎編**: 音をデジタルで扱う基本と、声の仕組み。ボイチェン編とデノイズ編を読むための土台です。
- **ボイチェン編**: 声の高さ（ピッチ）と声色（フォルマント）を変える 18 の方式。
- **デノイズ編**: 声からノイズを取り除く 11 の方式と、その前後の段。

数式はできるだけ使わず、図と言葉で説明します。各ページの終わりに、VoiChanAlgoBox で試すときの「聞きどころ」を付けています。

## 基礎編

1. [音とデジタル](basics/01-sound.md) — 波形、サンプリング、dB、少しずつ処理する仕組み
2. [周波数とスペクトル](basics/02-spectrum.md) — フーリエ変換、窓、スペクトログラム、時間と周波数の細かさ
3. [声の仕組み](basics/03-voice.md) — 声帯の音と口・喉のフィルタ（ソース・フィルタモデル）、倍音、フォルマント
4. [ピッチとフォルマント](basics/04-pitch-and-formant.md) — 男声を女声にするとき、何を変えるのか
5. [声色の形（包絡）の求め方](basics/05-envelope.md) — LPC、ケプストラム、True Envelope、メルケプストラム
6. [ピッチ検出](basics/06-pitch-detection.md) — 声の高さを見つける 10 の方式
7. [遅延](basics/07-latency.md) — リアルタイムの処理で、なぜ音が遅れるのか

## ボイチェン編

1. [方式の地図](voice/01-overview.md) — 18 方式の分類と、出力の比べ方
2. [ディレイライン式と WSOLA 式](voice/02-delay-line-wsola.md) — 早回しで高さを変える
3. [TD-PSOLA](voice/03-td-psola.md) — 声の 1 周期ずつ並べ直す
4. [フェーズボコーダと正弦波モデル](voice/04-phase-vocoder-sinusoidal.md) — 周波数の成分を動かす
5. [周波数シフト](voice/05-frequency-shift.md) — 全体を同じ Hz だけずらす
6. [声色と声帯の音に分ける方式](voice/06-source-filter.md) — LPC・ケプストラム・メルケプ ×（ディレイライン / PSOLA）
7. [ボコーダ](voice/07-vocoders.md) — 声帯の音を作り直す
8. [WORLD](voice/08-world.md) — 声を分解して作り直す
9. [フォルマントの動かし方と抑揚](voice/09-formant-warp.md) — 一律 / 低い側を残す / 帯域ごと / フォルマントごと

## デノイズ編

1. [ノイズとデノイズの地図](denoise/01-overview.md) — ノイズの種類、段の分担、11 方式の比べ方
2. [ノイズの見積もり](denoise/02-noise-estimation.md) — 何がノイズかを知る
3. [帯域分割式とくし形フィルタ](denoise/03-band-split.md) — 遅延 0 で下げる
4. [FFT の方式](denoise/04-fft.md) — 下げる量の決め方、ミュージカルノイズ、ならし方
5. [遅延 0 でかける工夫](denoise/05-zero-latency.md) — FFT＋FIR、＋帯域分割
6. [WebRTC NS と AI の方式](denoise/06-webrtc-ai.md) — WebRTC NS、RNNoise、DeepFilterNet
7. [前後の段（ノッチ・リペア・ゲート）](denoise/07-related-stages.md) — デノイズの苦手を受け持つ

## 図について

図は `Tools/Docs/make_figures.py` で作っています（入力は合成の声と、Windows の音声合成の声）。出力のスペクトログラムは、VoiChanAlgoBox と同じ処理を WAV にかける開発用のツール（vcab_render）で作った実際の結果です。
