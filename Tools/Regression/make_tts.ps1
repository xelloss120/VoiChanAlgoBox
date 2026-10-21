# 回帰テストと評価で使う声（build/regression/tts.wav）を、Windows の音声合成（日本語の声）で作る
Add-Type -AssemblyName System.Speech
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$dir = Join-Path $repo 'build\regression'
New-Item -ItemType Directory -Force $dir | Out-Null
$out = Join-Path $dir 'tts.wav'

$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$voice = $s.GetInstalledVoices() | Where-Object { $_.VoiceInfo.Culture.Name -eq 'ja-JP' } | Select-Object -First 1
if ($null -eq $voice) { throw '日本語の音声合成の声が入っていません' }
$s.SelectVoice($voice.VoiceInfo.Name)
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo 48000, ([System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen), ([System.Speech.AudioFormat.AudioChannel]::Mono)
$s.SetOutputToWaveFile($out, $fmt)
$s.Speak('こんにちは。今日はボイスチェンジャーのデノイズを試しています。サ行の、さしすせそ、と、息の音も確かめます。ノイズが消えて、声だけが残れば成功です。')
$s.Dispose()
"作った: $out"
