VoiChanAlgoBox 1.0.0
====================

いろいろなボイスチェンジャーのアルゴリズムを切り替えて、聞き比べられる
ボイスチェンジャーです（男性の声を女性の声に変える使い方を想定）。
対応: Windows（64bit）

■ 中身
  VoiChanAlgoBox.exe   単体で動く EXE 版
  VoiChanAlgoBox.vst3  VST3 プラグイン

■ 使い方
  EXE 版: VoiChanAlgoBox.exe を実行し、左上の「オプション」→
          「オーディオ設定...」で入力と出力のデバイスを選びます。
  VST3 版: VoiChanAlgoBox.vst3 をフォルダごと
          C:\Program Files\Common Files\VST3 にコピーし、DAW で読み込みます。

■ 注意
  ・ヘッドフォンで使ってください。スピーカーだとハウリング（大きな音）が
    起きることがあります。EXE 版は起動してすぐ音が出ます。
  ・署名をしていないため、初回に「PC が保護されました」と出たら
    「詳細情報」→「実行」を押してください。
  ・EXE 版の設定は %APPDATA%\VoiChanAlgoBox に保存されます。

■ 画面
  右上の「表示」で 簡単 / 普通 / 詳細 を切り替えられます。
  詳しい説明は、つまみや項目にマウスを乗せると出ます。

■ ライセンス
  Copyright (C) 2026 x120
  AGPLv3（LICENSE.txt）。使っているライブラリのライセンスは
  THIRD_PARTY_LICENSES*.md にあります（GPLv3 の本文は GPL-3.0.txt）。
  この版のソースコード:
    https://github.com/xelloss120/VoiChanAlgoBox/releases/tag/v1.0.0
  VST is a trademark of Steinberg Media Technologies GmbH.