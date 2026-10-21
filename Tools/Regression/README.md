# 回帰テストと評価

`vcab_render`（WAV を処理するツール）で決まった入力を処理し、音がビット単位で変わっていないかを確かめます。整理（リファクタリング）の前後や、ほかの処理を直したときに使います。必要なもの: Python 3、numpy、scipy。

1. `powershell -File make_tts.ps1` で、テスト用の声（Windows の日本語の音声合成）を `build/regression/tts.wav` に作る（一度だけ）
2. 変更の前に `python regress.py make` で基準を作る（`build/regression/ref`）
3. 変更してビルドしたら `python regress.py check`（`ずれ 0 件` なら同じ音）

音が変わるのが意図どおりなら、`make` で基準を作り直します。名前の一部を並べると、その場合だけを処理します（例: `python regress.py check fft_ tdPsola`）。

`denoise_eval.py` は、デノイズの効き方を測るツールです。ノイズの下がり方と、声・語頭の削れ方を dB で出します（例: `python denoise_eval.py denoise_method=3`）。

`tooltip_lines.py` は、画面の説明文（ツールチップ）が 3 行に収まっているかを数えるツールです（3 行を超えるものの数を出す）。
