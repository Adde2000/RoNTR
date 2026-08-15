// radio_dsp.hpp — portable audio DSP for radio effect + proximity gain/pan.
// No platform deps; unit-testable off-target.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace rtr {

// --- Biquad bandpass (RBJ cookbook), processes int16 in place ---------------
class Biquad {
public:
    void setupBandpass(float sampleRate, float centerHz, float q)
    {
        const float w0 = 2.0f * 3.14159265f * centerHz / sampleRate;
        const float alpha = std::sin(w0) / (2.0f * q);
        const float cw = std::cos(w0);
        const float a0 = 1.0f + alpha;
        b0 =  alpha / a0;
        b1 =  0.0f;
        b2 = -alpha / a0;
        a1 = -2.0f * cw / a0;
        a2 = (1.0f - alpha) / a0;
    }
    float process(float x)
    {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
private:
    float b0{}, b1{}, b2{}, a1{}, a2{};
    float x1{}, x2{}, y1{}, y2{};
};

// --- Radio voice effect: bandpass + drive + noise ---------------------------
// One instance per transmitting client (keeps filter state per stream).
class RadioEffect {
public:
    explicit RadioEffect(float sampleRate = 48000.0f)
    {
        // Two cascaded bandpasses approximate a 300–3400 Hz comms band.
        bp1.setupBandpass(sampleRate, 1100.0f, 0.7f);
        bp2.setupBandpass(sampleRate, 1100.0f, 0.7f);
    }

    // samples: interleaved int16, `count` frames, `channels` channels.
    void process(int16_t* samples, int count, int channels,
                 float drive = 4.0f, float noiseLevel = 0.02f)
    {
        for (int i = 0; i < count; ++i) {
            float x = samples[i * channels] / 32768.0f;
            x = step(x, drive, noiseLevel);
            const int16_t out = toI16(x);
            for (int c = 0; c < channels; ++c)
                samples[i * channels + c] = out; // radio is mono in both ears
        }
    }

    // mono float buffer in [-1, 1], processed in place.
    void processMono(float* x, int count, float drive = 4.0f, float noiseLevel = 0.02f)
    {
        for (int i = 0; i < count; ++i) x[i] = step(x[i], drive, noiseLevel);
    }

private:
    float step(float x, float drive, float noiseLevel)
    {
        x = bp2.process(bp1.process(x));
        x = softClip(x * drive);
        x += noiseLevel * whiteNoise();
        return x * 0.9f;
    }
    static float softClip(float x) { return std::tanh(x); }
    static float whiteNoise()
    {
        return (static_cast<float>(std::rand()) / RAND_MAX) * 2.0f - 1.0f;
    }
    static int16_t toI16(float x)
    {
        return static_cast<int16_t>(std::clamp(x, -1.0f, 1.0f) * 32767.0f);
    }
    Biquad bp1, bp2;
};

// --- Occlusion: one-pole lowpass + attenuation, slewed ----------------------
// Voice through walls sounds muffled and quieter; occlusion o in [0,1] comes
// from the game mod's wall count (RtrPlayer::occlusion / 255).

// One-pole lowpass: cheap and click-free while the cutoff moves. Default
// coefficient of 1 passes input through unchanged (the o=0 state).
class OnePoleLP {
public:
    void setCutoff(float sampleRate, float cutoffHz)
    {
        a = 1.0f - std::exp(-2.0f * 3.14159265f * cutoffHz / sampleRate);
    }
    float process(float x) { y += a * (x - y); return y; }
private:
    float a = 1.0f;
    float y = 0.0f;
};

struct OcclusionParams {
    float bypassHz      = 18000.0f; // o=0 cutoff (transparent for voice)
    float minCutoffHz   = 500.0f;   // o=1 cutoff (fully muffled)
    float maxAtten      = 0.55f;    // gain *= 1 - maxAtten*o on top of distance
    // Occlusion changes slowly (~150 ms full swing) so strafing past a corner
    // fades instead of popping. Deliberately much slower than PanState's slew.
    float slewPerSample = 1.0f / (0.150f * 48000.0f);
};

// Cutoff for occlusion o: log interpolation bypass -> minCutoff.
inline float occlusionCutoffHz(float o, const OcclusionParams& p = {})
{
    return std::exp(std::log(p.bypassHz) +
                    (std::log(p.minCutoffHz) - std::log(p.bypassHz)) * o);
}

// Per-stream state: caller passes the SAME struct across frames.
struct OcclusionState {
    OnePoleLP lp;
    float o = 0.0f; // slewed occlusion actually applied
};

// mono float buffer in [-1,1], processed in place.
inline void applyOcclusion(float* mono, int count, float targetO,
                           OcclusionState& st, float sampleRate = 48000.0f,
                           const OcclusionParams& p = {})
{
    targetO = std::clamp(targetO, 0.0f, 1.0f);
    for (int i = 0; i < count; ++i) {
        const float prev = st.o;
        st.o += std::clamp(targetO - st.o, -p.slewPerSample, p.slewPerSample);
        if (st.o != prev) // steady state skips the exp/log work
            st.lp.setCutoff(sampleRate, occlusionCutoffHz(st.o, p));
        mono[i] = st.lp.process(mono[i]) * (1.0f - p.maxAtten * st.o);
    }
}

// --- Proximity: distance gain + constant-power stereo pan -------------------
struct ProximityParams {
    float maxDistM = 40.0f;
    float rolloff  = 1.5f;
};

inline float distanceGain(float distM, const ProximityParams& p = {})
{
    if (distM >= p.maxDistM) return 0.0f;
    return std::pow(1.0f - distM / p.maxDistM, p.rolloff);
}

// pan in [-1 left, +1 right]; applies gain+pan to interleaved stereo (or mono).
// Smoothing: caller passes the SAME state struct per stream across frames.
struct PanState { float gain = 1.0f, pan = 0.0f; };

inline void applyProximity(int16_t* samples, int count, int channels,
                           float targetGain, float targetPan, PanState& st)
{
    const float smooth = 0.002f; // per-sample slew, avoids zipper noise
    for (int i = 0; i < count; ++i) {
        st.gain += std::clamp(targetGain - st.gain, -smooth, smooth);
        st.pan  += std::clamp(targetPan  - st.pan,  -smooth, smooth);
        const float theta = (st.pan + 1.0f) * 0.25f * 3.14159265f; // 0..pi/2
        const float gl = st.gain * std::cos(theta);
        const float gr = st.gain * std::sin(theta);
        if (channels >= 2) {
            samples[i * channels]     = static_cast<int16_t>(samples[i * channels]     * gl);
            samples[i * channels + 1] = static_cast<int16_t>(samples[i * channels + 1] * gr);
        } else {
            samples[i] = static_cast<int16_t>(samples[i] * st.gain);
        }
    }
}

} // namespace rtr
