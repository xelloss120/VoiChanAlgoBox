#include "FrequencyShifter.h"

#include "../Dsp/Biquad.h"

#include <cmath>

namespace vcab
{

double FrequencyShifter::shiftHz (const ParamValues& p)
{
    return p.get (P::fsReferenceHz) * (semitonesToRatio (p.get (P::pitch)) - 1.0);
}

void FrequencyShifter::prepare (double sampleRate, int)
{
    sr = sampleRate;
    reset();
}

void FrequencyShifter::reset()
{
    hilbert.reset();
    phase = 0;
}

void FrequencyShifter::process (float* data, int n, const ParamValues& params)
{
    const double step = 2 * kPi * shiftHz (params) / sr;
    for (int i = 0; i < n; ++i)
    {
        double inI = 0, inQ = 0;
        hilbert.process (data[i], inI, inQ);

        // 片側波帯変調: I cos + Q sin で全部の周波数が上へずれる（step が負なら下へ）
        data[i] = static_cast<float> (inI * std::cos (phase) + inQ * std::sin (phase));
        phase += step;
        if (phase > kPi) phase -= 2 * kPi;
        if (phase < -kPi) phase += 2 * kPi;
    }
}

} // namespace vcab
