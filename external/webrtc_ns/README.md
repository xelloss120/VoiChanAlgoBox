# WebRTC のノイズ抑制（抜き出し）

VoiChanAlgoBox のデノイズの方式「WebRTC NS」で使う、WebRTC の音声処理（Audio Processing Module）のノイズ抑制の部分だけを抜き出したものです。

- 元: freedesktop の webrtc-audio-processing（PulseAudio が使っている、WebRTC の音声処理の切り出し版）
  https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing （コミット d0569cf、2026-10-20 に取得）
- ライセンス: BSD 3 条項（`LICENSE`）と、特許の許諾（`PATENTS`）。FFT は Ooura の FFT（`webrtc/common_audio/third_party/ooura/LICENSE`）
- ライブラリ全体は Meson でビルドし、abseil を使うので、Windows の CMake のビルドにそのままは入らない。そのため、必要なファイルだけをコピーした（サブモジュールにしないのは、元のリポジトリにパスの長いファイルがあり、Windows で取り出しに失敗するため）

## 入っているもの（元のまま）

- `webrtc/modules/audio_processing/ns/`（ノイズ抑制。BUILD.gn は除く）
- `webrtc/modules/audio_processing/three_band_filter_bank.*`（48kHz を 3 つの帯域に分けて戻すフィルタ）
- `webrtc/common_audio/third_party/ooura/fft_size_256/`（FFT）
- `webrtc/api/array_view.h`、`webrtc/rtc_base/type_traits.h`

## VoiChanAlgoBox が置き換えたもの

- `webrtc/rtc_base/checks.h`: 元は abseil を使うので、デバッグ時だけ働く確認（RTC_DCHECK 系）を assert にした小さなものに置き換えた
- `webrtc/modules/audio_processing/audio_buffer.h`: 元のクラスは多くのファイルを連れてくるので、ノイズ抑制が使う `split_bands` / `split_bands_const` だけを持つ小さなものに置き換えた
