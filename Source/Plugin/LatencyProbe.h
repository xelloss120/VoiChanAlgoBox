// 遅延の実測を、裏のスレッドで行う（画面に出すため）
//
// 画面が「この設定で測って」と頼むと、裏のスレッドが measureLatencyMs を呼んで結果を置く。
// 測っている間に新しい頼みが来たら、古い結果は捨てて、新しい設定で測り直す。
#pragma once

#include "LatencyMeter.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class LatencyProbe
{
public:
    static constexpr float kMeasuring = -2.0f; // 測っている途中
    static constexpr float kFailed = -1.0f;    // 測れなかった（出力がほぼ無音など）

    LatencyProbe()
    {
        worker = std::thread ([this] { run(); });
    }

    ~LatencyProbe()
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            stop = true;
        }
        wake.notify_all();
        worker.join();
    }

    void request (const vcab::ParamValues& params, double sampleRate)
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            pending = params;
            pendingRate = sampleRate;
            ++requestId;
        }
        resultMs.store (kMeasuring);
        wake.notify_one();
    }

    float result() const noexcept { return resultMs.load(); }

private:
    void run()
    {
        int done = 0;
        for (;;)
        {
            vcab::ParamValues params;
            double rate = 48000;
            int id = 0;
            {
                std::unique_lock<std::mutex> lock (mutex);
                wake.wait (lock, [&] { return stop || requestId != done; });
                if (stop)
                    return;
                params = pending;
                rate = pendingRate;
                id = requestId;
            }

            const double ms = vcab::measureLatencyMs (params, rate);

            std::lock_guard<std::mutex> lock (mutex);
            done = id;
            if (requestId == id) // 測っている間に新しい頼みが来ていなければ、結果を置く
                resultMs.store (ms < 0 ? kFailed : static_cast<float> (ms));
        }
    }

    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    vcab::ParamValues pending;
    double pendingRate = 48000;
    int requestId = 0;
    std::atomic<float> resultMs { kMeasuring };
};
