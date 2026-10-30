#include "audio_limiter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace frontline::audio {

namespace {

int16_t Clamp16(float v) {
    long r = std::lrint(v);
    return (int16_t)(r < -0x8000 ? -0x8000 : r > 0x7FFF ? 0x7FFF : r);
}

}  // namespace

Limiter::Limiter(float rate) : m_release(1.0 - std::exp(-1.0 / (kReleaseSeconds * rate))) { Reset(); }

void Limiter::Reset() {
    for (int i = 0; i < kLookahead; i++) {
        m_delay[i][0] = m_delay[i][1] = 0;
        m_need[i] = 1.0f;
        m_held[i] = 1.0;
    }
    m_pos = 0;
    m_heldSum = kLookahead;
    m_heldBelow = 0;
    m_released = 1.0;
    m_gain = 1.0f;
}

void Limiter::Process(const int32_t* left, const int32_t* right, int16_t (*out)[2], int count) {
    for (int i = 0; i < count; i++) {
        // This frame's need, and the smallest over the lookahead.
        float peak = (float)std::max(std::abs(left[i]), std::abs(right[i]));
        m_need[m_pos] = peak > kCeiling ? kCeiling / peak : 1.0f;
        float need = *std::min_element(m_need, m_need + kLookahead);

        // Down at once to the need held over the lookahead, back up slowly.
        if (need < m_released) {
            m_released = need;
        } else if (m_released < 1.0) {
            m_released += (need - m_released) * m_release;
            if (m_released > 0.99999) {
                m_released = 1.0;
            }
        }

        // Smoothed over the lookahead: its average reaches each peak's need
        // as that peak leaves the delay.
        double old = m_held[m_pos];
        m_heldSum += m_released - old;
        m_heldBelow += (m_released < 1.0) - (old < 1.0);
        m_held[m_pos] = m_released;
        m_gain = m_heldBelow == 0 ? 1.0f : std::min(1.0f, (float)(m_heldSum / kLookahead));

        // The frame entering now leaves kLookahead - 1 frames later: the
        // oldest in the ring, the slot after this one.
        int oldest = (m_pos + 1) % kLookahead;
        m_delay[m_pos][0] = left[i];
        m_delay[m_pos][1] = right[i];
        if (m_gain == 1.0f) {
            out[i][0] = (int16_t)std::clamp(m_delay[oldest][0], -0x8000, 0x7FFF);
            out[i][1] = (int16_t)std::clamp(m_delay[oldest][1], -0x8000, 0x7FFF);
        } else {
            out[i][0] = Clamp16(m_delay[oldest][0] * m_gain);
            out[i][1] = Clamp16(m_delay[oldest][1] * m_gain);
        }
        m_pos = oldest;
    }
}

}  // namespace frontline::audio
