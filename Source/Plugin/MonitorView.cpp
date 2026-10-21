#include "MonitorView.h"

namespace
{
const juce::Colour kInputColour { 0xff4fa3e0 };  // 入力: 青
const juce::Colour kOutputColour { 0xfff0a040 }; // 出力: 橙
const juce::Colour kCutColour { 0xffd05050 };    // カットの線: 赤

juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

// スペクトログラムの色（暗い青 → 紫 → 橙 → 黄）
juce::Colour heat (float t)
{
    t = juce::jlimit (0.0f, 1.0f, t);
    static const juce::Colour stops[] = { juce::Colour (0xff000010), juce::Colour (0xff301060), juce::Colour (0xffa02070),
                                          juce::Colour (0xfff07030), juce::Colour (0xfffff0a0) };
    const float x = t * 4.0f;
    const int i = juce::jmin (3, static_cast<int> (x));
    return stops[i].interpolatedWith (stops[i + 1], x - static_cast<float> (i));
}
} // namespace

MonitorView::MonitorView (VoiChanAlgoBoxProcessor& p)
    : processor (p)
{
    // 背景は自分で全部塗るので、描き直すときに後ろの画面まで描き直さなくてよい（軽くするため）
    setOpaque (true);

    window.resize (fftSize);
    // ハン窓（表示用。固定）
    for (int i = 0; i < fftSize; ++i)
        window[static_cast<size_t> (i)] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * static_cast<float> (i) / fftSize);

    fftBuffer.resize (2 * fftSize);

    for (size_t i = 0; i < heatLut.size(); ++i)
        heatLut[i] = heat (static_cast<float> (i) / static_cast<float> (heatLut.size() - 1)).getPixelARGB();
}

void MonitorView::resized()
{
    auto r = getLocalBounds();
    const int mode = processor.currentParams().index (vcab::P::monitorMode);

    spectrumArea = {};
    inSpecArea = {};
    outSpecArea = {};

    switch (mode)
    {
        case vcab::monitorSpectrum:       spectrumArea = r; break;
        case vcab::monitorInSpectrogram:  inSpecArea = r; break;
        case vcab::monitorOutSpectrogram: outSpecArea = r; break;
        case vcab::monitorBothSpectrograms:
            inSpecArea = r.removeFromLeft (r.getWidth() / 2).withTrimmedRight (4);
            outSpecArea = r.withTrimmedLeft (4);
            break;
        case vcab::monitorSpectrumAndIn:
            spectrumArea = r.removeFromLeft (r.getWidth() / 2).withTrimmedRight (4);
            inSpecArea = r.withTrimmedLeft (4);
            break;
        case vcab::monitorSpectrumAndOut:
            spectrumArea = r.removeFromLeft (r.getWidth() / 2).withTrimmedRight (4);
            outSpecArea = r.withTrimmedLeft (4);
            break;
        case vcab::monitorAll:
            spectrumArea = r.removeFromLeft (r.getWidth() / 2).withTrimmedRight (4);
            inSpecArea = r.removeFromLeft (r.getWidth() / 2).withTrimmedLeft (4).withTrimmedRight (2);
            outSpecArea = r.withTrimmedLeft (2);
            break;
        default: break; // 非表示（モニター自体は画面の側で隠す）
    }
    repaint();

    for (auto* ch : { &input, &output })
    {
        const auto& a = ch == &input ? inSpecArea : outSpecArea;
        if (! a.isEmpty() && (ch->spectrogram.getWidth() != a.getWidth() || ch->spectrogram.getHeight() != a.getHeight()))
        {
            // GPU（Direct2D）上の画像に1点ずつ書き込むのはとても遅いので、CPU 側の画像にする
            ch->spectrogram = juce::Image (juce::Image::RGB, juce::jmax (1, a.getWidth()), juce::jmax (1, a.getHeight()), true,
                                           juce::SoftwareImageType());
            ch->spectrogram.clear (ch->spectrogram.getBounds(), heat (0));
            ch->writeX = 0;
        }
    }
}

void MonitorView::update()
{
    // モニターの表示の切り替えが変わったら、配置し直す
    const int mode = processor.currentParams().index (vcab::P::monitorMode);
    if (mode != lastMonitorMode)
    {
        lastMonitorMode = mode;
        resized();
    }

    processor.inputAnalysis.pull (pulled);
    if (! spectrumArea.isEmpty() || ! inSpecArea.isEmpty())
        analyse (input, pulled);
    processor.outputAnalysis.pull (pulled);
    if (! spectrumArea.isEmpty() || ! outSpecArea.isEmpty())
        analyse (output, pulled);

    // 非表示のときは、分析も描画もしない（溜まった音は捨てる）
    if (! spectrumArea.isEmpty() || ! inSpecArea.isEmpty() || ! outSpecArea.isEmpty())
        repaint();
}

void MonitorView::analyse (Channel& ch, const std::vector<float>& incoming)
{
    for (float s : incoming)
    {
        ch.history[static_cast<size_t> (ch.writePos)] = s;
        ch.writePos = (ch.writePos + 1) % fftSize;

        if (++ch.sinceLastFrame >= hopSize)
        {
            ch.sinceLastFrame = 0;
            computeFrame (ch, frameScratch);
            scrollSpectrogram (ch, frameScratch);
        }
    }
}

void MonitorView::computeFrame (Channel& ch, std::vector<float>& frameDb)
{
    std::fill (fftBuffer.begin(), fftBuffer.end(), 0.0f);
    // リングバッファを古い順に並べ直しながら窓をかける
    for (int i = 0; i < fftSize; ++i)
        fftBuffer[static_cast<size_t> (i)] = ch.history[static_cast<size_t> ((ch.writePos + i) % fftSize)] * window[static_cast<size_t> (i)];

    fft.performFrequencyOnlyForwardTransform (fftBuffer.data(), true);

    // 正弦波の振幅が 0dB になるように正規化（ハン窓の平均 0.5 と、片側スペクトルの 2倍）
    const float norm = 2.0f / (fftSize * 0.5f);
    for (int k = 0; k <= fftSize / 2; ++k)
    {
        const float mag = fftBuffer[static_cast<size_t> (k)] * norm;
        const float db = juce::Decibels::gainToDecibels (mag, minDb);
        frameDb[static_cast<size_t> (k)] = db;

        // スペクトルの線は、上がるときは速く、下がるときはゆっくり
        auto& s = ch.spectrumDb[static_cast<size_t> (k)];
        s = db > s ? db : s + (db - s) * 0.15f;
    }
}

void MonitorView::scrollSpectrogram (Channel& ch, const std::vector<float>& frameDb)
{
    auto& img = ch.spectrogram;
    if (! img.isValid())
        return;

    // 絵全体をずらさずに、書き込む列の位置だけを進める（描くときに2つに分けて並べる）
    const int w = img.getWidth(), h = img.getHeight();
    const double sr = processor.currentSampleRate.load();

    juce::Image::BitmapData bd (img, ch.writeX, 0, 1, h, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < h; ++y)
    {
        // 縦軸も対数（上が高い周波数）
        const float t = 1.0f - static_cast<float> (y) / static_cast<float> (juce::jmax (1, h - 1));
        const float hz = minHz * std::pow (maxHz / minHz, t);
        const int bin = juce::jlimit (0, fftSize / 2, static_cast<int> (hz / sr * fftSize + 0.5));
        const float level = (frameDb[static_cast<size_t> (bin)] - minDb) / (maxDb - minDb);
        const auto idx = static_cast<size_t> (juce::jlimit (0, 255, static_cast<int> (level * 255.0f)));
        bd.setPixelColour (0, y, juce::Colour (heatLut[idx]));
    }

    ch.writeX = (ch.writeX + 1) % w;
}

float MonitorView::xForHz (float hz, juce::Rectangle<float> area) const
{
    const float t = std::log (hz / minHz) / std::log (maxHz / minHz);
    return area.getX() + t * area.getWidth();
}

float MonitorView::yForDb (float db, juce::Rectangle<float> area) const
{
    const float t = (db - minDb) / (maxDb - minDb);
    return area.getBottom() - t * area.getHeight();
}

void MonitorView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0f1724));

    if (! spectrumArea.isEmpty())
        drawSpectrum (g, spectrumArea);
    if (! inSpecArea.isEmpty())
        drawSpectrogram (g, inSpecArea, input, utf8 ("入力"));
    if (! outSpecArea.isEmpty())
        drawSpectrogram (g, outSpecArea, output, utf8 ("出力"));
}

void MonitorView::drawFrequencyGrid (juce::Graphics& g, juce::Rectangle<float> area, bool labels)
{
    g.setFont (juce::FontOptions (17.0f));
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = xForHz (hz, area);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawVerticalLine (static_cast<int> (x), area.getY(), area.getBottom());
        if (labels)
        {
            g.setColour (juce::Colours::white.withAlpha (0.4f));
            const auto text = hz >= 1000 ? juce::String (hz / 1000, 0) + "k" : juce::String (static_cast<int> (hz));
            g.drawText (text, juce::Rectangle<float> (x + 2, area.getBottom() - 22, 50, 20), juce::Justification::left);
        }
    }
}

void MonitorView::drawSpectrum (juce::Graphics& g, juce::Rectangle<int> areaInt)
{
    const auto area = areaInt.toFloat();
    g.setColour (juce::Colour (0xff15202f));
    g.fillRect (area);

    drawFrequencyGrid (g, area, true);

    for (float db : { -80.0f, -60.0f, -40.0f, -20.0f })
    {
        const float y = yForDb (db, area);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawHorizontalLine (static_cast<int> (y), area.getX(), area.getRight());
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.setFont (juce::FontOptions (17.0f));
        g.drawText (juce::String (static_cast<int> (db)), juce::Rectangle<float> (area.getX() + 2, y - 20, 44, 20), juce::Justification::left);
    }

    const double sr = processor.currentSampleRate.load();

    auto drawCurve = [&] (const Channel& ch, juce::Colour colour)
    {
        juce::Path path;
        bool started = false;
        const int w = static_cast<int> (area.getWidth());
        for (int px = 0; px < w; px += 2)
        {
            const float t = static_cast<float> (px) / static_cast<float> (w);
            const float hz = minHz * std::pow (maxHz / minHz, t);
            const int bin = juce::jlimit (1, fftSize / 2, static_cast<int> (hz / sr * fftSize + 0.5));
            const float y = yForDb (ch.spectrumDb[static_cast<size_t> (bin)], area);
            const float x = area.getX() + static_cast<float> (px);
            if (! started) { path.startNewSubPath (x, y); started = true; }
            else path.lineTo (x, y);
        }
        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (1.5f));
    };

    drawCurve (input, kInputColour);
    drawCurve (output, kOutputColour);

    const auto p = processor.effectiveParams();

    // デノイズのノイズの見積もり（緑の点線）。見積もりは「白色雑音に直したときの 1 サンプルあたりの 2 乗」なので、
    // この表示（ハン窓の fftSize 点、大きさ × 4 / fftSize）に直す: 白色雑音の分散 σ² は 10log10(σ² × 6 / fftSize) dB になる。
    // 表示のスペクトルは dB の平均でならしているので、ノイズは 2 乗の平均より約 2.5dB 低く見える。その分も引く
    {
        auto& m = processor.meters();
        if (p.on (vcab::P::denoiseOn) && p.on (vcab::P::showNoiseEstimate) && m.noiseCurveValid.load())
        {
            const float offset = 10.0f * std::log10 (6.0f / fftSize) - 2.5f;
            juce::Path path;
            for (int i = 0; i < vcab::EngineMeters::kNoiseCurvePoints; ++i)
            {
                const float x = xForHz (vcab::EngineMeters::noiseCurveHz (i), area);
                const float y = yForDb (m.noiseCurveDb[i].load() + offset, area);
                if (i == 0) path.startNewSubPath (x, y);
                else path.lineTo (x, y);
            }
            juce::Path dashed;
            const float dashes[] = { 6.0f, 4.0f };
            juce::PathStrokeType (2.0f).createDashedStroke (dashed, path, dashes, 2);
            g.setColour (juce::Colour (0xff60d080));
            g.fillPath (dashed);
            g.setFont (juce::FontOptions (17.0f));
            g.drawText (utf8 ("ノイズの見積もり"), juce::Rectangle<float> (area.getX() + 50, area.getY() + 4, 200, 20), juce::Justification::left);
        }
    }

    // ディエッサーが見ている帯域を薄く色付けする
    if (p.on (vcab::P::deessOn))
    {
        const float x = xForHz (p.get (vcab::P::deessFreq), area);
        g.setColour (juce::Colour (0xffd070c0).withAlpha (0.10f));
        g.fillRect (juce::Rectangle<float> (x, area.getY(), area.getRight() - x, area.getHeight()));
        g.setColour (juce::Colour (0xffd070c0).withAlpha (0.8f));
        g.setFont (juce::FontOptions (17.0f));
        g.drawText (utf8 ("ディエッサー"), juce::Rectangle<float> (x + 3, area.getBottom() - 46, 120, 20), juce::Justification::left);
    }

    // カット（ボイチェンの前）の縦線と、傾きの線（縦軸は EQ の線と同じ ±18dB。切り始める所から下だけを描く）
    if (std::abs (cutSampleRate - sr) > 1)
    {
        lowCutForDrawing.prepare (sr);
        highCutForDrawing.prepare (sr);
        cutSampleRate = sr;
    }
    auto cutLine = [&] (bool on, float hz, const char* label, int row, bool high)
    {
        if (! on) return;
        const float x = xForHz (hz, area);
        g.setColour (kCutColour.withAlpha (0.8f));
        g.drawVerticalLine (static_cast<int> (x), area.getY(), area.getBottom());
        auto& filter = high ? highCutForDrawing : lowCutForDrawing;
        filter.setParams (hz, p.index (high ? vcab::P::highCutSlope : vcab::P::lowCutSlope));
        juce::Path path;
        bool started = false;
        const int w = static_cast<int> (area.getWidth());
        for (int px = 0; px <= w; px += 2)
        {
            const float xx = area.getX() + static_cast<float> (px);
            const float db = juce::Decibels::gainToDecibels (static_cast<float> (filter.magnitudeAt (hzForX (xx, area))), -100.0f);
            if (db > -0.2f) { started = false; continue; } // 素通しの所は描かない（EQ の 0dB の線と重ならないように）
            const float y = eqYForDb (db, area);
            if (! started) { path.startNewSubPath (xx, y); started = true; }
            else path.lineTo (xx, y);
        }
        g.strokePath (path, juce::PathStrokeType (2.0f));
        g.setFont (juce::FontOptions (17.0f));
        const float y = area.getY() + 26.0f + 20.0f * static_cast<float> (row);
        // ハイカットは線の左、ローカットは線の右に書く
        if (high)
            g.drawText (utf8 (label), juce::Rectangle<float> (x - 102, y, 100, 20), juce::Justification::right);
        else
            g.drawText (utf8 (label), juce::Rectangle<float> (x + 3, y, 100, 20), juce::Justification::left);
    };
    const bool cutOn = p.on (vcab::P::cutOn);
    cutLine (cutOn && p.on (vcab::P::lowCutOn), p.get (vcab::P::lowCutFreq), "ローカット", 0, false);
    cutLine (cutOn && p.on (vcab::P::highCutOn), p.get (vcab::P::highCutFreq), "ハイカット", 0, true);

    // ノッチの位置（自動は ▼、ハムは細い縦線）
    {
        auto& m = processor.meters();
        g.setColour (juce::Colour (0xffe0c040));
        const int count = m.notchCount.load();
        for (int i = 0; i < count; ++i)
        {
            const float x = xForHz (m.notchHz[i].load(), area);
            juce::Path tri;
            tri.addTriangle (x - 6, area.getY() + 2, x + 6, area.getY() + 2, x, area.getY() + 12);
            g.fillPath (tri);
        }
        const float mains = m.humMainsHz.load();
        if (mains > 0)
        {
            g.setColour (juce::Colour (0xffe0c040).withAlpha (0.35f));
            for (int h = 1; h <= static_cast<int> (p.get (vcab::P::humHarmonics)); ++h)
                if (mains * h >= minHz && mains * h <= maxHz)
                    g.drawVerticalLine (static_cast<int> (xForHz (mains * h, area)), area.getY(), area.getY() + 14);
        }
    }

    // EQ の線と点
    drawEq (g, area, p);

    // 凡例
    g.setFont (juce::FontOptions (17.0f));
    // 凡例はグラフの上の真ん中
    const float cx = area.getCentreX();
    g.setColour (kInputColour);
    g.drawText (utf8 ("入力"), juce::Rectangle<float> (cx - 58, area.getY() + 3, 54, 20), juce::Justification::right);
    g.setColour (kOutputColour);
    g.drawText (utf8 ("出力"), juce::Rectangle<float> (cx + 4, area.getY() + 3, 54, 20), juce::Justification::left);
}

void MonitorView::drawSpectrogram (juce::Graphics& g, juce::Rectangle<int> area, const Channel& ch, const juce::String& title)
{
    if (ch.spectrogram.isValid())
    {
        // 古い列（writeX〜右端）を左に、新しい列（0〜writeX）を右に並べる
        const auto& img = ch.spectrogram;
        const int w = img.getWidth(), h = img.getHeight();
        const int oldW = w - ch.writeX;
        g.drawImage (img, area.getX(), area.getY(), oldW, h, ch.writeX, 0, oldW, h);
        if (ch.writeX > 0)
            g.drawImage (img, area.getX() + oldW, area.getY(), ch.writeX, h, 0, 0, ch.writeX, h);
    }

    // 周波数の目盛り（横線）
    g.setFont (juce::FontOptions (17.0f));
    for (float hz : { 100.0f, 1000.0f, 10000.0f })
    {
        const float t = std::log (hz / minHz) / std::log (maxHz / minHz);
        const float y = static_cast<float> (area.getBottom()) - t * static_cast<float> (area.getHeight());
        g.setColour (juce::Colours::white.withAlpha (0.15f));
        g.drawHorizontalLine (static_cast<int> (y), static_cast<float> (area.getX()), static_cast<float> (area.getRight()));
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.drawText (hz >= 1000 ? juce::String (hz / 1000, 0) + "k" : juce::String (static_cast<int> (hz)),
                    juce::Rectangle<float> (static_cast<float> (area.getX()) + 2, y - 20, 50, 20), juce::Justification::left);
    }

    g.setColour (juce::Colours::white.withAlpha (0.8f));
    g.setFont (juce::FontOptions (17.0f));
    g.drawText (title, area.reduced (4).removeFromTop (22), juce::Justification::centred);
}
