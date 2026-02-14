#ifndef FLUX_DSP_H
#define FLUX_DSP_H

// Pure DSP functions for Flux 8-voice drift oscillator.
// No Disting NT API dependencies — testable on desktop.

#include <math.h>
#include <stdint.h>

namespace flux {

static constexpr float TWO_PI = 6.283185307179586f;

// Denormal protection: flush subnormals to zero
inline void flush_denormal( float& x )
{
    if ( fabsf( x ) < 1e-10f )
        x = 0.0f;
}

// DC blocker: 1-pole highpass filter at ~20Hz
struct DCBlocker
{
    float prevInput = 0.0f;
    float prevOutput = 0.0f;
    float R = 0.999f;  // Pole for ~20Hz at 48kHz

    float process( float input )
    {
        float output = input - prevInput + R * prevOutput;
        prevInput = input;
        flush_denormal( output );
        prevOutput = output;
        return output;
    }
};

// Independent drift generator per voice.
// Uses xorshift32 PRNG feeding a one-pole lowpass filter
// to produce slowly varying random pitch deviation.
struct DriftGenerator
{
    uint32_t rng;
    float filtered;

    void init( uint32_t seed )
    {
        rng = seed | 1;  // Ensure non-zero (xorshift requirement)
        filtered = 0.0f;
    }

    // Returns slowly varying value approximately in [-1, 1].
    // alpha controls rate of change (~0.0001 at 48kHz gives ~1Hz drift).
    float process( float alpha )
    {
        // xorshift32 PRNG
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;

        // Convert to bipolar float [-1, 1)
        float noise = (float)(int32_t)rng * 4.6566129e-10f;

        // One-pole lowpass: smooths white noise into slow random walk
        filtered += alpha * ( noise - filtered );

        // Scale to approximately [-1, 1] range and soft-clamp.
        // With alpha ~0.0001, raw filtered std ~0.007.
        // Scale by 130 to fill ±1 range, then clamp.
        float out = filtered * 130.0f;
        if ( out > 1.0f ) out = 1.0f;
        else if ( out < -1.0f ) out = -1.0f;

        return out;
    }
};

// Fast cents-to-multiplier for small deviations (±100 cents).
// Uses linear approximation: 2^(c/1200) ≈ 1 + c * ln(2)/1200
// Error < 0.04% within ±50 cents.
inline float cents_to_mult_fast( float cents )
{
    return 1.0f + cents * 0.00057762265f;  // ln(2)/1200
}

// Compute sine from normalized phase [0, 1)
inline float oscillator_sine( float phase )
{
    return sinf( phase * TWO_PI );
}

// Advance phase by increment, wrap to [0, 1)
inline void phase_advance( float& phase, float increment )
{
    phase += increment;
    phase -= floorf( phase );
}

// MIDI note to frequency. Note 69 = A4 = 440Hz.
inline float midi_note_to_freq( uint8_t note )
{
    return 440.0f * exp2f( ( (float)note - 69.0f ) / 12.0f );
}

// V/OCT to frequency. 0V = C4 (261.63Hz), 1V/octave.
inline float voct_to_freq( float voltage )
{
    return 261.63f * exp2f( voltage );
}

// --- Raw waveform generators from normalized phase [0, 1) ---

inline float waveform_triangle( float phase )
{
    if ( phase < 0.25f )
        return phase * 4.0f;
    else if ( phase < 0.75f )
        return 2.0f - phase * 4.0f;
    else
        return phase * 4.0f - 4.0f;
}

inline float waveform_saw( float phase )
{
    return 2.0f * phase - 1.0f;
}

inline float waveform_pulse( float phase )
{
    return phase < 0.5f ? 1.0f : -1.0f;
}

// --- PolyBLEP anti-aliasing ---

// PolyBLEP correction for discontinuities.
// phase: normalized [0, 1), dt: phase increment per sample.
inline float polyblep( float phase, float dt )
{
    if ( phase < dt )
    {
        float t = phase / dt;
        return t + t - t * t - 1.0f;
    }
    else if ( phase > 1.0f - dt )
    {
        float t = ( phase - 1.0f ) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// PolyBLEP-corrected saw
inline float waveform_saw_blep( float phase, float dt )
{
    return waveform_saw( phase ) - polyblep( phase, dt );
}

// PolyBLEP-corrected pulse
inline float waveform_pulse_blep( float phase, float dt )
{
    float p = waveform_pulse( phase );
    p += polyblep( phase, dt );               // Rising edge at 0
    float shifted = phase + 0.5f;
    if ( shifted >= 1.0f ) shifted -= 1.0f;
    p -= polyblep( shifted, dt );             // Falling edge at 0.5
    return p;
}

// --- Waveform morphing ---

// Wave warp: morph sine -> triangle -> saw -> pulse
// phase: normalized [0, 1), warp: 0.0-1.0
inline float wave_warp( float phase, float warp )
{
    if ( warp <= 0.0f )
        return oscillator_sine( phase );

    float sine = oscillator_sine( phase );

    if ( warp <= 1.0f / 3.0f )
    {
        float t = warp * 3.0f;
        float tri = waveform_triangle( phase );
        return sine + t * ( tri - sine );
    }
    else if ( warp <= 2.0f / 3.0f )
    {
        float t = ( warp - 1.0f / 3.0f ) * 3.0f;
        float tri = waveform_triangle( phase );
        float saw = waveform_saw( phase );
        return tri + t * ( saw - tri );
    }
    else
    {
        float t = ( warp - 2.0f / 3.0f ) * 3.0f;
        float saw = waveform_saw( phase );
        float pls = waveform_pulse( phase );
        return saw + t * ( pls - saw );
    }
}

// Wave warp with PolyBLEP anti-aliasing for saw and pulse stages
inline float wave_warp_blep( float phase, float warp, float dt )
{
    if ( warp <= 0.0f )
        return oscillator_sine( phase );

    float sine = oscillator_sine( phase );

    if ( warp <= 1.0f / 3.0f )
    {
        float t = warp * 3.0f;
        float tri = waveform_triangle( phase );
        return sine + t * ( tri - sine );
    }
    else if ( warp <= 2.0f / 3.0f )
    {
        float t = ( warp - 1.0f / 3.0f ) * 3.0f;
        float tri = waveform_triangle( phase );
        float saw = waveform_saw_blep( phase, dt );
        return tri + t * ( saw - tri );
    }
    else
    {
        float t = ( warp - 2.0f / 3.0f ) * 3.0f;
        float saw = waveform_saw_blep( phase, dt );
        float pls = waveform_pulse_blep( phase, dt );
        return saw + t * ( pls - saw );
    }
}

} // namespace flux

#endif // FLUX_DSP_H
