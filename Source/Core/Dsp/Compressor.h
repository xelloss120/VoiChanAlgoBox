// コンプレッサー。先読みなし（遅延 0）。メイクアップはなし（出力ゲインで代わりにする）。
#pragma once

#include <algorithm>
#include <cmath>

namespace vcab
{

class Compressor
{
public:
    struct Settings
    {
        float thresholdDb = -24, ratio = 2, attackMs = 10, releaseMs = 120, kneeDb = 6;
    };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        envCoef = static_cast<float> (std::exp (-1.0 / (0.005 * sr))); // 判断用の音量は 5ms でならす
        reset();
    }

    void reset()
    {
        envSq = 0;
        grDb = 0;
        levelDb = -120;
    }

    void setSettings (const Settings& s) noexcept
    {
        settings = s;
        attackCoef = static_cast<float> (std::exp (-1.0 / (std::max (0.05f, s.attackMs) * 0.001 * sr)));
        releaseCoef = static_cast<float> (std::exp (-1.0 / (std::max (1.0f, s.releaseMs) * 0.001 * sr)));
    }

    // 入力の音量 x（dB）に対する、出力の音量（dB）。ニー付き。画面の線にも使う
    static float staticCurve (float x, const Settings& s) noexcept
    {
        const float t = s.thresholdDb, r = std::max (1.0f, s.ratio), w = s.kneeDb;
        const float over = x - t;
        if (2 * over < -w)
            return x;
        if (w > 0 && 2 * std::abs (over) <= w)
            return x + (1 / r - 1) * (over + w / 2) * (over + w / 2) / (2 * w);
        return t + over / r;
    }

    void process (float* data, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float x = data[i];
            envSq = envCoef * envSq + (1 - envCoef) * x * x;
            levelDb = 10.0f * std::log10 (envSq + 1.0e-12f);

            const float target = staticCurve (levelDb, settings) - levelDb; // 下げる量（0 以下）
            const float coef = target < grDb ? attackCoef : releaseCoef;
            grDb = coef * grDb + (1 - coef) * target;

            data[i] = x * std::pow (10.0f, grDb / 20.0f);
        }
    }

    // 表示用
    float inputLevelDb() const noexcept { return levelDb; }
    float gainReductionDb() const noexcept { return grDb; }

private:
    double sr = 48000;
    Settings settings;
    float envSq = 0, envCoef = 0, attackCoef = 0, releaseCoef = 0;
    float grDb = 0, levelDb = -120;
};

} // namespace vcab
