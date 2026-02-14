#include <new>
#include <math.h>
#include <string.h>
#include <distingnt/api.h>
#include "dsp.h"

static const int NUM_VOICES = 8;

// --- Algorithm struct ---

struct _fluxAlgorithm : public _NT_algorithm
{
    // Phase state (free-running, never reset on Note-On)
    float phase[NUM_VOICES];

    // Drift generators (one per voice, independent random walks)
    flux::DriftGenerator drift[NUM_VOICES];

    // Cached parameters (set by parameterChanged)
    uint8_t oscEnabled[NUM_VOICES];      // 0=off, 1=on
    float oscTransposeMult[NUM_VOICES];  // frequency multiplier from semitones
    float oscShape[NUM_VOICES];          // 0.0-1.0

    float driftAmount;       // 0.0-1.0
    float globalVCA;         // 0.0-1.0
    float fineTune;          // frequency multiplier from cents

    // MIDI state
    float baseFrequency;     // Hz, from V/OCT or MIDI
    float pitchBendFactor;   // multiplier (1.0 = no bend)
    uint8_t midiNote;        // current MIDI note number
    uint8_t midiGate;        // 1=on, 0=off
    uint8_t midiChannel;     // 0-15

    flux::DCBlocker dcBlocker;

    _fluxAlgorithm()
    {
        memset( phase, 0, sizeof(phase) );

        // Initialize drift generators with unique seeds per voice
        for ( int i = 0; i < NUM_VOICES; ++i )
        {
            drift[i].init( 0xDEADBEEF + (uint32_t)i * 0x13579BDF );
            oscEnabled[i] = 1;           // All voices on by default
            oscTransposeMult[i] = 1.0f;  // Unison
            oscShape[i] = 0.0f;          // Sine
        }

        driftAmount = 0.0f;
        globalVCA = 1.0f;
        fineTune = 1.0f;
        baseFrequency = 261.63f;   // C4
        pitchBendFactor = 1.0f;
        midiNote = 60;
        midiGate = 0;
        midiChannel = 0;
    }
};

// --- Parameter indices ---

enum {
    // I/O
    kParamOutput,
    kParamOutputMode,

    // Global
    kParamDrift,
    kParamFineTune,
    kParamMidiChannel,
    kParamGlobalVCA,
    kParamVersion,

    // Per-Oscillator (3 each x 8)
    kParamOsc1Enable, kParamOsc1Transpose, kParamOsc1Shape,
    kParamOsc2Enable, kParamOsc2Transpose, kParamOsc2Shape,
    kParamOsc3Enable, kParamOsc3Transpose, kParamOsc3Shape,
    kParamOsc4Enable, kParamOsc4Transpose, kParamOsc4Shape,
    kParamOsc5Enable, kParamOsc5Transpose, kParamOsc5Shape,
    kParamOsc6Enable, kParamOsc6Transpose, kParamOsc6Shape,
    kParamOsc7Enable, kParamOsc7Transpose, kParamOsc7Shape,
    kParamOsc8Enable, kParamOsc8Transpose, kParamOsc8Shape,

    // CV Inputs
    kParamVOctCV,
    kParamDriftCV,
    kParamGlobalVCACV,

    kNumParams
};

// Helper: parameter index for oscillator N (0-based), offset within block
static inline int oscParam( int osc, int offset ) { return kParamOsc1Enable + osc * 3 + offset; }
enum { kOscEnable = 0, kOscTranspose = 1, kOscShape = 2 };

// --- Enum strings ---

static const char* offOnStrings[] = { "Off", "On", NULL };
static const char* versionStrings[] = { FLUX_VERSION, NULL };

// --- Parameter definitions ---

#define OSC_PARAMS(n) \
    { "Osc" #n " Enable",    0,   1,   1, kNT_unitEnum,      0, offOnStrings }, \
    { "Osc" #n " Transpose", -48, 48,  0, kNT_unitSemitones, 0, NULL }, \
    { "Osc" #n " Shape",     0,  100,  0, kNT_unitPercent,   0, NULL },

static _NT_parameter parameters[] = {
    // I/O
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output", 1, 13 )

    // Global
    { "Drift",        0, 100,   0, kNT_unitPercent, 0, NULL },
    { "Fine Tune", -100, 100,   0, kNT_unitCents,   0, NULL },
    { "MIDI Channel", 1,  16,   1, kNT_unitNone,    0, NULL },
    { "Global VCA",   0, 100, 100, kNT_unitPercent, 0, NULL },

    // Version (read-only)
    { "Version",      0,   0,   0, kNT_unitEnum,    0, versionStrings },

    // Oscillators
    OSC_PARAMS(1) OSC_PARAMS(2) OSC_PARAMS(3) OSC_PARAMS(4)
    OSC_PARAMS(5) OSC_PARAMS(6) OSC_PARAMS(7) OSC_PARAMS(8)

    // CV Inputs
    NT_PARAMETER_CV_INPUT( "V/OCT CV",      0, 0 )
    NT_PARAMETER_CV_INPUT( "Drift CV",      0, 0 )
    NT_PARAMETER_CV_INPUT( "Global VCA CV", 0, 0 )
};

// --- Parameter pages ---

static const uint8_t pageIO[] = { kParamOutput, kParamOutputMode };
static const uint8_t pageGlobal[] = {
    kParamDrift, kParamFineTune, kParamGlobalVCA, kParamVersion
};
static const uint8_t pageMIDI[] = { kParamMidiChannel };

#define OSC_PAGE(n) \
    static const uint8_t pageOsc##n[] = { \
        kParamOsc##n##Enable, kParamOsc##n##Transpose, kParamOsc##n##Shape \
    };
OSC_PAGE(1) OSC_PAGE(2) OSC_PAGE(3) OSC_PAGE(4)
OSC_PAGE(5) OSC_PAGE(6) OSC_PAGE(7) OSC_PAGE(8)

static const uint8_t pageCV[] = { kParamVOctCV, kParamDriftCV, kParamGlobalVCACV };

static const _NT_parameterPage pages[] = {
    { .name = "I/O",    .numParams = ARRAY_SIZE(pageIO),      .params = pageIO },
    { .name = "Global", .numParams = ARRAY_SIZE(pageGlobal),  .params = pageGlobal },
    { .name = "MIDI",   .numParams = ARRAY_SIZE(pageMIDI),    .params = pageMIDI },
    { .name = "Osc 1",  .numParams = ARRAY_SIZE(pageOsc1),    .group = 10, .params = pageOsc1 },
    { .name = "Osc 2",  .numParams = ARRAY_SIZE(pageOsc2),    .group = 10, .params = pageOsc2 },
    { .name = "Osc 3",  .numParams = ARRAY_SIZE(pageOsc3),    .group = 10, .params = pageOsc3 },
    { .name = "Osc 4",  .numParams = ARRAY_SIZE(pageOsc4),    .group = 10, .params = pageOsc4 },
    { .name = "Osc 5",  .numParams = ARRAY_SIZE(pageOsc5),    .group = 10, .params = pageOsc5 },
    { .name = "Osc 6",  .numParams = ARRAY_SIZE(pageOsc6),    .group = 10, .params = pageOsc6 },
    { .name = "Osc 7",  .numParams = ARRAY_SIZE(pageOsc7),    .group = 10, .params = pageOsc7 },
    { .name = "Osc 8",  .numParams = ARRAY_SIZE(pageOsc8),    .group = 10, .params = pageOsc8 },
    { .name = "CV",     .numParams = ARRAY_SIZE(pageCV),       .params = pageCV },
};

static const _NT_parameterPages parameterPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages = pages,
};

// --- MIDI CC mapping ---

// CC14-41 -> 28 value parameters (excludes CV bus selectors and version)
// 4 global + 24 per-osc (3x8)
static const int8_t ccToParam[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,                     // 0-13
    kParamDrift, kParamFineTune, kParamMidiChannel, kParamGlobalVCA, // 14-17
    kParamOsc1Enable, kParamOsc1Transpose, kParamOsc1Shape,          // 18-20
    kParamOsc2Enable, kParamOsc2Transpose, kParamOsc2Shape,          // 21-23
    kParamOsc3Enable, kParamOsc3Transpose, kParamOsc3Shape,          // 24-26
    kParamOsc4Enable, kParamOsc4Transpose, kParamOsc4Shape,          // 27-29
    kParamOsc5Enable, kParamOsc5Transpose, kParamOsc5Shape,          // 30-32
    kParamOsc6Enable, kParamOsc6Transpose, kParamOsc6Shape,          // 33-35
    kParamOsc7Enable, kParamOsc7Transpose, kParamOsc7Shape,          // 36-38
    kParamOsc8Enable, kParamOsc8Transpose, kParamOsc8Shape,          // 39-41
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,               // 42-57
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,               // 58-73
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,               // 74-89
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,               // 90-105
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,               // 106-121
    -1,-1,-1,-1,-1,-1                                                // 122-127
};

// Scale CC value (0-127) to parameter's min..max range
static int16_t scaleCCToParam( uint8_t ccValue, int paramIndex )
{
    int16_t mn = parameters[paramIndex].min;
    int16_t mx = parameters[paramIndex].max;
    return mn + (int16_t)( (int32_t)ccValue * ( mx - mn ) / 127 );
}

// --- Lifecycle ---

static void calculateRequirements(
    _NT_algorithmRequirements& req,
    const int32_t* specifications )
{
    req.numParameters = ARRAY_SIZE(parameters);
    req.sram = sizeof( _fluxAlgorithm );
    req.dram = 0;
    req.dtc = 0;
    req.itc = 0;
}

static _NT_algorithm* construct(
    const _NT_algorithmMemoryPtrs& ptrs,
    const _NT_algorithmRequirements& req,
    const int32_t* specifications )
{
    _fluxAlgorithm* alg = new ( ptrs.sram ) _fluxAlgorithm();
    alg->parameters = parameters;
    alg->parameterPages = &parameterPages;
    return alg;
}

// --- Parameter changed ---

static void parameterChanged( _NT_algorithm* self, int parameter )
{
    _fluxAlgorithm* p = (_fluxAlgorithm*)self;

    // Per-oscillator parameters
    for ( int osc = 0; osc < NUM_VOICES; ++osc )
    {
        int base = kParamOsc1Enable + osc * 3;
        if ( parameter >= base && parameter < base + 3 )
        {
            int offset = parameter - base;
            switch ( offset )
            {
            case kOscEnable:
                p->oscEnabled[osc] = p->v[parameter];
                break;
            case kOscTranspose:
                // Convert semitones to frequency multiplier: 2^(semi/12)
                p->oscTransposeMult[osc] = exp2f( (float)p->v[parameter] / 12.0f );
                break;
            case kOscShape:
                p->oscShape[osc] = (float)p->v[parameter] * 0.01f;
                break;
            }
            return;
        }
    }

    // Global parameters
    switch ( parameter )
    {
    case kParamDrift:
        p->driftAmount = (float)p->v[parameter] * 0.01f;
        break;
    case kParamFineTune:
        p->fineTune = exp2f( (float)p->v[parameter] / 1200.0f );
        break;
    case kParamMidiChannel:
        p->midiChannel = p->v[parameter] - 1;  // 1-16 -> 0-15
        break;
    case kParamGlobalVCA:
        p->globalVCA = (float)p->v[parameter] * 0.01f;
        break;
    }
}

// --- Audio ---

static void step(
    _NT_algorithm* self,
    float* busFrames,
    int numFramesBy4 )
{
    _fluxAlgorithm* p = (_fluxAlgorithm*)self;
    int numFrames = numFramesBy4 * 4;

    float* out = busFrames + ( p->v[kParamOutput] - 1 ) * numFrames;
    bool replace = p->v[kParamOutputMode];

    float sampleRate = (float)NT_globals.sampleRate;
    // Drift filter coefficient: ~0.0001 at 48kHz gives ~1Hz rate of change
    float driftAlpha = 4.8f / sampleRate;

    // Read CV buses (0 = not connected)
    const float* cvVOct = p->v[kParamVOctCV]
        ? busFrames + ( p->v[kParamVOctCV] - 1 ) * numFrames : NULL;
    const float* cvDrift = p->v[kParamDriftCV]
        ? busFrames + ( p->v[kParamDriftCV] - 1 ) * numFrames : NULL;
    const float* cvGlobalVCA = p->v[kParamGlobalVCACV]
        ? busFrames + ( p->v[kParamGlobalVCACV] - 1 ) * numFrames : NULL;

    // Count active voices for normalization (constant within one step call)
    int numActive = 0;
    for ( int v = 0; v < NUM_VOICES; ++v )
        if ( p->oscEnabled[v] ) numActive++;

    float normFactor = numActive > 0 ? 1.0f / (float)numActive : 0.0f;

    for ( int i = 0; i < numFrames; ++i )
    {
        // Base frequency: MIDI overrides V/OCT when gate is on
        float baseFreq = p->baseFrequency;
        if ( cvVOct && !p->midiGate )
            baseFreq = flux::voct_to_freq( cvVOct[i] );

        float freq = baseFreq * p->pitchBendFactor * p->fineTune;

        // Drift amount with CV modulation
        float driftAmt = p->driftAmount;
        if ( cvDrift )
            driftAmt = fminf( 1.0f, fmaxf( 0.0f, driftAmt + cvDrift[i] * 0.2f ) );

        float mix = 0.0f;

        for ( int v = 0; v < NUM_VOICES; ++v )
        {
            // Always advance drift even for disabled voices.
            // This keeps each voice's random walk evolving independently,
            // preventing artifacts when re-enabled.
            float driftVal = p->drift[v].process( driftAlpha );

            // Skip audio calculations for disabled voices (CPU optimization)
            if ( !p->oscEnabled[v] ) continue;

            // Compute drift pitch deviation (up to +/-50 cents at max drift)
            float driftCents = driftVal * driftAmt * 50.0f;
            float driftMult = flux::cents_to_mult_fast( driftCents );

            // Voice frequency with transpose and drift
            float voiceFreq = freq * p->oscTransposeMult[v] * driftMult;
            if ( voiceFreq < 0.0f ) voiceFreq = 0.0f;

            float inc = voiceFreq / sampleRate;

            // Advance phase (free-running, never reset)
            flux::phase_advance( p->phase[v], inc );

            // Generate waveform with PolyBLEP anti-aliasing
            float sample;
            if ( p->oscShape[v] > 0.0f )
                sample = flux::wave_warp_blep( p->phase[v], p->oscShape[v], inc );
            else
                sample = flux::oscillator_sine( p->phase[v] );

            mix += sample;
        }

        // Normalize by active voice count to maintain consistent output level
        mix *= normFactor;

        // Global VCA with CV
        float vca = p->globalVCA;
        if ( cvGlobalVCA )
            vca *= fmaxf( 0.0f, cvGlobalVCA[i] * 0.2f );
        mix *= vca;

        // DC blocking
        mix = p->dcBlocker.process( mix );

        // Output
        if ( replace )
            out[i] = mix;
        else
            out[i] += mix;
    }
}

// --- MIDI ---

static void midiMessage(
    _NT_algorithm* self,
    uint8_t byte0,
    uint8_t byte1,
    uint8_t byte2 )
{
    _fluxAlgorithm* p = (_fluxAlgorithm*)self;

    uint8_t status  = byte0 & 0xF0;
    uint8_t channel = byte0 & 0x0F;

    if ( channel != p->midiChannel )
        return;

    switch ( status )
    {
    case 0x90:  // Note On
        if ( byte2 > 0 )
        {
            p->midiNote = byte1;
            p->midiGate = 1;
            p->baseFrequency = flux::midi_note_to_freq( byte1 );
            // No phase reset — free-running oscillators
        }
        else
        {
            // Velocity 0 = note off
            if ( byte1 == p->midiNote )
                p->midiGate = 0;
        }
        break;

    case 0x80:  // Note Off
        if ( byte1 == p->midiNote )
            p->midiGate = 0;
        break;

    case 0xB0:  // Control Change
    {
        if ( byte1 >= 128 ) break;
        int8_t paramIdx = ccToParam[byte1];
        if ( paramIdx >= 0 )
        {
            int16_t value = scaleCCToParam( byte2, paramIdx );
            NT_setParameterFromAudio( NT_algorithmIndex(self), paramIdx, value );
        }
        break;
    }

    case 0xE0:  // Pitch Bend
    {
        int16_t bend = ( (int16_t)byte2 << 7 ) | byte1;  // 0-16383
        float bendNorm = (float)( bend - 8192 ) / 8192.0f;  // -1 to +1
        // +/-2 semitones pitch bend range
        p->pitchBendFactor = exp2f( bendNorm * 2.0f / 12.0f );
        break;
    }
    }
}

// --- Factory ---

static const _NT_factory factory = {
    .guid = NT_MULTICHAR('F', 'l', 'u', 'x'),
    .name = "Flux",
    .description = "Flux v" FLUX_VERSION " - 8-voice drift oscillator",
    .numSpecifications = 0,
    .specifications = NULL,
    .calculateStaticRequirements = NULL,
    .initialise = NULL,
    .calculateRequirements = calculateRequirements,
    .construct = construct,
    .parameterChanged = parameterChanged,
    .step = step,
    .draw = NULL,
    .midiRealtime = NULL,
    .midiMessage = midiMessage,
    .tags = kNT_tagInstrument,
    .hasCustomUi = NULL,
    .customUi = NULL,
    .setupUi = NULL,
    .serialise = NULL,
    .deserialise = NULL,
    .midiSysEx = NULL,
    .parameterUiPrefix = NULL,
    .parameterString = NULL,
};

// --- Entry point ---

extern "C"
uintptr_t pluginEntry( _NT_selector selector, uint32_t data )
{
    switch ( selector )
    {
    case kNT_selector_version:
        return kNT_apiVersionCurrent;
    case kNT_selector_numFactories:
        return 1;
    case kNT_selector_factoryInfo:
        return (uintptr_t)( ( data == 0 ) ? &factory : NULL );
    }
    return 0;
}
