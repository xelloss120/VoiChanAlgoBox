// vcab_render: 開発用の WAV 処理ツール
//
// WAV を読み込み（または合成音を作り）、本体と同じ Engine で小さな塊ずつ処理して、WAV で書き出す。
//
// 使い方の例:
//   vcab_render --in voice.wav --out out.wav --set pitch=12 --set formant=0
//   vcab_render --gen sine:200 --dur 2 --out sine_up.wav --set pitch=12
//   vcab_render --in voice.wav --outdir results --all
//   vcab_render --gen speech:120 --pitch pitch.csv   （ピッチ検出だけを、すべての方式で試す）
//   vcab_render --list   （パラメータの一覧）
#include "../../Source/Core/Dsp/FormantTracker.h"
#include "../../Source/Core/Dsp/PitchDetector.h"
#include "../../Source/Core/Engine.h"
#include "../../Source/Core/LatencyMeter.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#if JUCE_WINDOWS
extern "C" __declspec (dllimport) int __stdcall SetConsoleOutputCP (unsigned int);
#endif

namespace
{

void printUsage()
{
    std::cout <<
        "vcab_render - VoiChanAlgoBox の開発用 WAV 処理ツール\n"
        "\n"
        "  --in <file.wav>        入力の WAV\n"
        "  --gen <kind>:<hz>      入力の代わりに合成音を作る（sine / pulse / sweep）。sweep は <hz> を無視\n"
        "  --dur <秒>             合成音の長さ（初期値 2）\n"
        "  --sr <Hz>              合成音のサンプルレート（初期値 48000）\n"
        "  --out <file.wav>       出力の WAV\n"
        "  --outdir <dir>         --all のときの出力先\n"
        "  --all                  全方式で処理して、方式ごとにファイルを書き出す\n"
        "  --set <key>=<値>       パラメータを指定（複数可）。choice と on/off は番号（0, 1, ...）\n"
        "  --block <サンプル数>   処理する塊の大きさ（初期値 128）\n"
        "  --save-input <file>    合成した入力も保存する\n"
        "  --learn-noise          始めの 2 秒でデノイズのノイズを学習する（ノイズの見積もりを「学習」にしたときの確認用）\n"
        "  --save-learned <file>  処理の後に、デノイズの学習した値をファイルに書き出す（保存の確認用）\n"
        "  --load-learned <file>  処理の前に、デノイズの学習した値をファイルから読み込む（保存の確認用）\n"
        "  --noise-curve          処理の最後の、デノイズのノイズの見積もり（表示用の値）を出す\n"
        "  --realtime             実際の時間の速さで処理する（重い方式の作業スレッドが締め切りに間に合うかを確かめる）\n"
        "  --measure              遅延を実測して、計算値と並べて表示する（--all と組み合わせると全方式）\n"
        "  --formants <file.csv>  フォルマントの追いかけだけを行い、塊ごとの F1・F2・F3 を書き出す\n"
        "  --list                 パラメータの一覧を表示\n";
}

void printParams()
{
    for (const auto& s : vcab::paramSpecs())
    {
        std::cout << s.key << "  [" << s.minValue << " .. " << s.maxValue << "]  初期値 " << s.defaultValue;
        if (! s.choices.empty())
        {
            std::cout << "  {";
            for (size_t i = 0; i < s.choices.size(); ++i)
                std::cout << (i ? ", " : "") << i << ":" << s.choices[i];
            std::cout << "}";
        }
        std::cout << "  " << s.name << "\n";
    }
}

// 声に近い合成音。パルス列（声帯の音）に、「あ」のフォルマント（800 / 1200 / 2500Hz）の共振をかける
juce::AudioBuffer<float> generateSpeech (double hz, double sr)
{
    const int n = static_cast<int> (sr * 3.5);
    juce::AudioBuffer<float> buf (1, n);
    auto* d = buf.getWritePointer (0);
    juce::Random rng (12345);

    struct Resonator
    {
        double a1 = 0, a2 = 0, g = 0, y1 = 0, y2 = 0;
        Resonator (double f, double bw, double sr)
        {
            const double r = std::exp (-juce::MathConstants<double>::pi * bw / sr);
            a1 = 2 * r * std::cos (2 * juce::MathConstants<double>::pi * f / sr);
            a2 = -r * r;
            g = 1 - r;
        }
        double process (double x) { const double y = g * x + a1 * y1 + a2 * y2; y2 = y1; y1 = y; return y; }
    };
    Resonator formants[] = { { 800, 80, sr }, { 1200, 90, sr }, { 2500, 120, sr } };
    double hiss1 = 0, ph = 0;

    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        double v = 0;
        if (t >= 0.5 && t < 1.5)
        {
            const double f = hz * (1 + 0.03 * std::sin (2 * juce::MathConstants<double>::pi * 5 * t)); // 5Hz のビブラート
            ph += f / sr;
            v = ph >= 1 ? 1.0 : 0.0;
        }
        else if (t >= 2.0 && t < 3.0)
        {
            const double f = hz * std::pow (2.0, t - 2.0);
            ph += f / sr;
            v = ph >= 1 ? 1.0 : 0.0;
        }
        ph -= std::floor (ph);

        double out = 0;
        for (auto& r : formants)
            out += r.process (v);
        out *= 1.5;

        if (t >= 1.5 && t < 2.0)
        {
            // 息（サ行のような高い雑音）。1次の差分で高域を強める
            const double w = rng.nextFloat() * 2.0 - 1.0;
            out += 0.15 * (w - hiss1);
            hiss1 = w;
        }
        d[i] = static_cast<float> (out);
    }
    return buf;
}

juce::AudioBuffer<float> generate (const juce::String& what, double sr, double seconds)
{
    const auto kind = what.upToFirstOccurrenceOf (":", false, false);
    const double hz = what.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
    if (kind == "speech")
        return generateSpeech (hz, sr);
    const int n = static_cast<int> (sr * seconds);

    juce::AudioBuffer<float> buf (1, n);
    auto* d = buf.getWritePointer (0);
    double ph = 0;
    juce::Random rng (12345);

    for (int i = 0; i < n; ++i)
    {
        float v = 0;
        if (kind == "noise")
        {
            // 白色雑音。値は RMS の dBFS（一様乱数の RMS は振幅の 1/√3）
            const float amp = juce::Decibels::decibelsToGain (static_cast<float> (hz)) * std::sqrt (3.0f);
            v = (rng.nextFloat() * 2.0f - 1.0f) * amp;
        }
        else if (kind == "sine")
        {
            v = static_cast<float> (0.5 * std::sin (2 * juce::MathConstants<double>::pi * ph));
            ph += hz / sr;
        }
        else if (kind == "pulse")
        {
            // 声帯の音を模した、倍音の多いパルス列（1周期に1回、幅の狭い山）
            v = ph < 0.02 ? 0.8f : 0.0f;
            ph += hz / sr;
            ph -= std::floor (ph);
        }
        else if (kind == "sweep")
        {
            // 50Hz から 20kHz まで、対数で上がる正弦波
            const double t = static_cast<double> (i) / n;
            const double f = 50.0 * std::pow (20000.0 / 50.0, t);
            v = static_cast<float> (0.5 * std::sin (2 * juce::MathConstants<double>::pi * ph));
            ph += f / sr;
        }
        ph -= std::floor (ph);
        d[i] = v;
    }
    return buf;
}

bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buf, double sr)
{
    file.deleteFile();
    file.getParentDirectory().createDirectory();
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;

    juce::WavAudioFormat wav;
    auto options = juce::AudioFormatWriterOptions{}
                       .withSampleRate (sr)
                       .withNumChannels (buf.getNumChannels())
                       .withBitsPerSample (32)
                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    std::unique_ptr<juce::OutputStream> os (stream.release());
    auto writer = wav.createWriterFor (os, options);
    if (writer == nullptr)
        return false;

    return writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

struct RenderResult
{
    juce::AudioBuffer<float> output;
    vcab::LatencyBreakdown latency;
    double seconds = 0;
    double maxBlockMs = 0; // 1塊の処理にかかった最長の時間（音の処理のスレッドの締め切りに間に合うかの目安）
    int overDeadline = 0;  // 塊の長さの時間（締め切り）を超えた回数
    float peakDb = -120;
};

static bool learnNoise = false; // --learn-noise
static juce::String saveLearnedPath, loadLearnedPath; // --save-learned / --load-learned
static bool printNoiseCurve = false; // --noise-curve

RenderResult render (const juce::AudioBuffer<float>& input, double sr, const vcab::ParamValues& params, int block, bool realtime = false)
{
    vcab::Engine engine;
    engine.prepare (sr, block);
    engine.setRealtime (realtime); // ふだんはその場で計算して、結果を毎回同じにする
    if (learnNoise)
        engine.requestNoiseLearning();
    if (loadLearnedPath.isNotEmpty())
    {
        juce::MemoryBlock data;
        juce::File (loadLearnedPath).loadFileAsData (data);
        std::vector<float> learned (data.getSize() / sizeof (float));
        std::memcpy (learned.data(), data.getData(), learned.size() * sizeof (float));
        engine.importLearnedNoise (std::move (learned)); // 次の process で反映される
    }

    RenderResult r;
    r.output = input;
    r.latency = engine.latency (params);

    auto* d = r.output.getWritePointer (0);
    const int n = r.output.getNumSamples();

    const auto t0 = std::chrono::steady_clock::now();
    for (int pos = 0; pos < n; pos += block)
    {
        if (realtime)
        {
            // オーディオの割り込みと同じく、塊の長さの時間ごとに処理する
            const auto due = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration> (std::chrono::duration<double> (pos / sr));
            while (std::chrono::steady_clock::now() < due)
                ;
        }
        const auto b0 = std::chrono::steady_clock::now();
        engine.process (d + pos, std::min (block, n - pos), params);
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - b0).count();
        r.maxBlockMs = std::max (r.maxBlockMs, ms);
        if (ms > 1000.0 * block / sr)
            ++r.overDeadline;
    }
    r.seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    if (printNoiseCurve)
        std::printf ("repair_counts clip=%d plosive=%d transient=%d click=%d crackle=%d breath=%d\n", engine.meters.repairClips.load(), engine.meters.repairPlosives.load(),
                     engine.meters.repairTransients.load(), engine.meters.repairClicks.load(), engine.meters.repairCrackles.load(),
                     engine.meters.repairBreaths.load());
    if (printNoiseCurve && engine.meters.noiseCurveValid.load())
        for (int i = 0; i < vcab::EngineMeters::kNoiseCurvePoints; ++i)
            std::printf ("noise_curve %.2f %.2f\n", vcab::EngineMeters::noiseCurveHz (i), engine.meters.noiseCurveDb[i].load());
    if (saveLearnedPath.isNotEmpty())
    {
        const auto learned = engine.exportLearnedNoise();
        juce::File (saveLearnedPath).replaceWithData (learned.data(), learned.size() * sizeof (float));
    }

    r.peakDb = juce::Decibels::gainToDecibels (r.output.getMagnitude (0, 0, n), -120.0f);
    return r;
}

void report (const juce::String& label, const RenderResult& r, double sr, int numSamples)
{
    const double audioSeconds = numSamples / sr;
    std::printf ("%-28s 遅延 %.1fms（ボイチェン %.1f / リペア %.1f / デノイズ %.1f / 出力 %.1f）  処理時間 %.3fs（リアルタイムの %.0f 倍速、1塊の最長 %.2fms、締め切り超え %d 回）  ピーク %.1fdB\n",
                 label.toRawUTF8(),
                 r.latency.total() * 1000.0 / sr,
                 r.latency.voice * 1000.0 / sr,
                 r.latency.repair * 1000.0 / sr,
                 r.latency.denoise * 1000.0 / sr,
                 r.latency.output * 1000.0 / sr,
                 r.seconds,
                 r.seconds > 0 ? audioSeconds / r.seconds : 0.0,
                 r.maxBlockMs,
                 r.overDeadline,
                 r.peakDb);
}

// フォルマントの追いかけだけを行う。CSV の列: 時刻（窓の中心）, F1, F2, F3, 追いかけたか, 倍音らしさ
int detectFormants (const juce::AudioBuffer<float>& input, double sr, const juce::File& csv, int block)
{
    vcab::FormantTracker tracker;
    tracker.prepare (sr);
    juce::String out ("time,f1,f2,f3,valid,voicing\n");
    const int n = input.getNumSamples();
    const auto* d = input.getReadPointer (0);
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = std::min (block, n - pos);
        tracker.process (d + pos, len);
        const auto& t = tracker.track();
        out << juce::String ((pos + len) / sr - 0.0125, 4) << "," << juce::String (t.hz[0], 1) << "," << juce::String (t.hz[1], 1) << ","
            << juce::String (t.hz[2], 1) << "," << (t.valid ? 1 : 0) << "," << juce::String (tracker.voicing(), 3) << "\n";
    }
    return csv.replaceWithText (out) ? 0 : 1;
}

// ピッチ検出だけを、すべての方式で行う。CSV の列: 時刻（窓の中心）, 方式ごとに 声の高さ, 有声, 確からしさ
int detectPitch (const juce::AudioBuffer<float>& input, double sr, const vcab::ParamValues& params, const juce::File& csv, int block)
{
    const int n = input.getNumSamples();
    const auto* d = input.getReadPointer (0);
    std::vector<std::vector<vcab::PitchEstimate>> results (vcab::PitchDetector::numMethods);
    std::vector<std::vector<double>> times (vcab::PitchDetector::numMethods); // 方式ごとの窓の中心の時刻

    for (int m = 0; m < vcab::PitchDetector::numMethods; ++m)
    {
        auto p = params;
        p.set (vcab::P::pitchMethod, static_cast<float> (m));
        vcab::PitchDetector det;
        det.prepare (sr);

        const auto t0 = std::chrono::steady_clock::now();
        for (int pos = 0; pos < n; pos += block)
        {
            // 塊の中で何回か検出し直すことがあるので、1サンプルずつ渡して結果をすべて拾う
            for (int i = pos; i < std::min (pos + block, n); ++i)
            {
                const int before = det.analysisCount();
                det.push (d + i, 1, p);
                if (det.analysisCount() != before)
                {
                    results[static_cast<size_t> (m)].push_back (det.latest());
                    times[static_cast<size_t> (m)].push_back ((i + 1 - det.analysisDelaySamples()) / sr);
                }
            }
        }
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();

        const auto& r = results[static_cast<size_t> (m)];
        std::vector<float> voicedHz;
        for (const auto& e : r)
            if (e.voiced)
                voicedHz.push_back (e.hz);
        std::sort (voicedHz.begin(), voicedHz.end());
        std::printf ("%-16s 有声 %5.1f%%  声の高さの中央値 %6.1fHz  処理時間 %.3fs（リアルタイムの %.0f 倍速）\n",
                     vcab::spec (vcab::P::pitchMethod).choices[static_cast<size_t> (m)],
                     r.empty() ? 0.0 : 100.0 * voicedHz.size() / r.size(),
                     voicedHz.empty() ? 0.0 : voicedHz[voicedHz.size() / 2],
                     secs, secs > 0 ? n / sr / secs : 0.0);
    }

    // time は1番目の方式の時刻（前の版と同じ）。方式ごとの窓の中心の時刻は time0, time1, ...（窓の長さが違うため）
    juce::String text ("time");
    for (int m = 0; m < vcab::PitchDetector::numMethods; ++m)
        text << ",time" << m << ",hz" << m << ",voiced" << m << ",clarity" << m;
    text << "\n";
    for (size_t k = 0; k < times[0].size(); ++k)
    {
        text << juce::String (times[0][k], 4);
        for (size_t m = 0; m < results.size(); ++m)
        {
            const auto& r = results[m];
            text << "," << juce::String (times[m][k], 4) << "," << juce::String (r[k].hz, 2) << "," << (r[k].voiced ? 1 : 0) << "," << juce::String (r[k].clarity, 3);
        }
        text << "\n";
    }
    return csv.replaceWithText (text) ? 0 : 1;
}

} // namespace

int main (int argc, char* argv[])
{
    // Windows のコンソールで日本語を正しく出す
   #if JUCE_WINDOWS
    SetConsoleOutputCP (65001);
   #endif

    juce::String inPath, outPath, outDir, gen, saveInput, pitchCsv, formantCsv;
    double dur = 2, genSr = 48000;
    int block = 128;
    bool all = false, realtime = false, measure = false;
    vcab::ParamValues params;
    bool applyMode = false;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a { juce::CharPointer_UTF8 (argv[i]) };
        auto next = [&]() -> juce::String { return i + 1 < argc ? juce::String (juce::CharPointer_UTF8 (argv[++i])) : juce::String(); };

        if (a == "--in") inPath = next();
        else if (a == "--out") outPath = next();
        else if (a == "--outdir") outDir = next();
        else if (a == "--gen") gen = next();
        else if (a == "--dur") dur = next().getDoubleValue();
        else if (a == "--sr") genSr = next().getDoubleValue();
        else if (a == "--block") block = juce::jmax (1, next().getIntValue());
        else if (a == "--save-input") saveInput = next();
        else if (a == "--all") all = true;
        else if (a == "--realtime") realtime = true;
        else if (a == "--learn-noise") learnNoise = true;
        else if (a == "--save-learned") saveLearnedPath = next();
        else if (a == "--load-learned") loadLearnedPath = next();
        else if (a == "--noise-curve") printNoiseCurve = true;
        else if (a == "--measure") measure = true;
        else if (a == "--pitch") pitchCsv = next();
        else if (a == "--formants") formantCsv = next();
        else if (a == "--list") { printParams(); return 0; }
        else if (a == "--set")
        {
            const auto kv = next();
            const auto key = kv.upToFirstOccurrenceOf ("=", false, false).toStdString();
            const int idx = vcab::findParamByKey (key);
            if (idx < 0)
            {
                std::cerr << "知らないパラメータ: " << key << "（--list で一覧）\n";
                return 1;
            }
            params.v[static_cast<size_t> (idx)] = kv.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
            if (key == "ui_mode")
                applyMode = true;
        }
        else { printUsage(); return a == "--help" ? 0 : 1; }
    }

    // ui_mode を指定したときだけ、プラグインと同じく、隠れる項目をオフや初期値にする（指定しなければ、すべての設定をそのまま使う）
    if (applyMode)
        params = vcab::applyDisplayMode (params);

    juce::AudioBuffer<float> input;
    double sr = genSr;

    if (gen.isNotEmpty())
    {
        input = generate (gen, sr, dur);
        if (saveInput.isNotEmpty())
            writeWav (juce::File::getCurrentWorkingDirectory().getChildFile (saveInput), input, sr);
    }
    else if (inPath.isNotEmpty())
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (juce::File::getCurrentWorkingDirectory().getChildFile (inPath)));
        if (reader == nullptr)
        {
            std::cerr << "読み込めません: " << inPath << "\n";
            return 1;
        }
        sr = reader->sampleRate;
        juce::AudioBuffer<float> raw (static_cast<int> (reader->numChannels), static_cast<int> (reader->lengthInSamples));
        reader->read (&raw, 0, raw.getNumSamples(), 0, true, true);

        // モノラルにまとめる
        input.setSize (1, raw.getNumSamples());
        input.clear();
        for (int ch = 0; ch < raw.getNumChannels(); ++ch)
            input.addFrom (0, 0, raw, ch, 0, raw.getNumSamples(), 1.0f / raw.getNumChannels());
    }
    else
    {
        printUsage();
        return 1;
    }

    const auto cwd = juce::File::getCurrentWorkingDirectory();

    if (pitchCsv.isNotEmpty())
        return detectPitch (input, sr, params, cwd.getChildFile (pitchCsv), block);
    if (formantCsv.isNotEmpty())
        return detectFormants (input, sr, cwd.getChildFile (formantCsv), block);

    if (all)
    {
        const auto& names = vcab::spec (vcab::P::algorithm).choices;
        const auto dir = cwd.getChildFile (outDir.isNotEmpty() ? outDir : juce::String ("render_all"));
        for (size_t a = 0; a < names.size(); ++a)
        {
            auto p = params;
            p.set (vcab::P::algorithm, static_cast<float> (a));
            const auto r = render (input, sr, p, block);
            const auto label = juce::String (a + 1).paddedLeft ('0', 2) + "_" + juce::String (juce::CharPointer_UTF8 (names[a]));
            writeWav (dir.getChildFile (label + ".wav"), r.output, sr);
            report (label, r, sr, input.getNumSamples());
            if (measure)
                std::printf ("%-28s 実測 %.1fms（計算値 %.1fms）\n", "", vcab::measureLatencyMs (p, sr), r.latency.total() * 1000.0 / sr);
        }
        return 0;
    }

    const auto r = render (input, sr, params, block, realtime);
    report ("result", r, sr, input.getNumSamples());
    if (measure)
        std::printf ("実測 %.1fms（計算値 %.1fms）\n", vcab::measureLatencyMs (params, sr), r.latency.total() * 1000.0 / sr);

    if (outPath.isNotEmpty() && ! writeWav (cwd.getChildFile (outPath), r.output, sr))
    {
        std::cerr << "書き出せません: " << outPath << "\n";
        return 1;
    }
    return 0;
}
