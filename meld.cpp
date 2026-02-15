#include <new>
#include <math.h>
#include <string.h>
#include <distingnt/api.h>
#include "dsp.h"

static const int NUM_VOICES = 8;

// --- Algorithm struct ---

struct _meldAlgorithm : public _NT_algorithm
{
    // Phase state (free-running, never reset on Note-On)
    float phase[NUM_VOICES];

    // Drift generators (one per voice, independent random walks)
    meld::DriftGenerator drift[NUM_VOICES];

    // Cached parameters (set by parameterChanged)
    uint8_t oscEnabled[NUM_VOICES];      // 0=off, 1=on
    float oscTransposeMult[NUM_VOICES];  // frequency multiplier from semitones
    float oscShape[NUM_VOICES];          // 0.0-1.0

    float driftAmount;       // 0.0-1.0
    float driftRateHz;       // 0.01-2.0 Hz
    float globalVCA;         // 0.0-1.0
    float fineTune;          // frequency multiplier from cents
    float globalBend;        // 0.0-1.0
    float globalFold;        // 0.0-1.0
    uint8_t oversample;      // 0=off, 1=2x
    float dsBuffer;          // downsample state

    // MIDI state
    float baseFrequency;     // Hz, from V/OCT or MIDI
    float pitchBendFactor;   // multiplier (1.0 = no bend)
    uint8_t midiNote;        // current MIDI note number
    uint8_t midiGate;        // 1=on, 0=off
    uint8_t midiChannel;     // 0-15

    meld::DCBlocker dcBlocker;

    _meldAlgorithm()
    {
        memset( phase, 0, sizeof(phase) );

        // Initialize drift generators with unique seeds per voice
        for ( int i = 0; i < NUM_VOICES; ++i )
        {
            drift[i].init( 0xDEADBEEF + (uint32_t)i * 0x13579BDF );
            oscEnabled[i] = ( i < 3 ) ? 1 : 0;  // Osc 1-3 on, 4-8 off
            oscTransposeMult[i] = 1.0f;           // Unison
            oscShape[i] = 0.667f;                 // Saw
        }

        driftAmount = 0.25f;
        driftRateHz = 0.10f;
        globalVCA = 1.0f;
        fineTune = 1.0f;
        globalBend = 0.0f;
        globalFold = 0.0f;
        oversample = 0;
        dsBuffer = 0.0f;
        baseFrequency = 261.63f;   // C4
        pitchBendFactor = 1.0f;
        midiNote = 60;
        midiGate = 0;
        midiChannel = 0;
    }
};

// --- Parameter indices ---

enum {
    // I/O (2)
    kParamOutput,
    kParamOutputMode,

    // Global (9)
    kParamDrift,
    kParamDriftRate,
    kParamFineTune,
    kParamOversampling,
    kParamGlobalBend,
    kParamGlobalFold,
    kParamMidiChannel,
    kParamGlobalVCA,
    kParamVersion,

    // Per-Oscillator (3 each x 8 = 24)
    kParamOsc1Enable, kParamOsc1Transpose, kParamOsc1Shape,
    kParamOsc2Enable, kParamOsc2Transpose, kParamOsc2Shape,
    kParamOsc3Enable, kParamOsc3Transpose, kParamOsc3Shape,
    kParamOsc4Enable, kParamOsc4Transpose, kParamOsc4Shape,
    kParamOsc5Enable, kParamOsc5Transpose, kParamOsc5Shape,
    kParamOsc6Enable, kParamOsc6Transpose, kParamOsc6Shape,
    kParamOsc7Enable, kParamOsc7Transpose, kParamOsc7Shape,
    kParamOsc8Enable, kParamOsc8Transpose, kParamOsc8Shape,

    // CV Inputs (5)
    kParamVOctCV,
    kParamDriftCV,
    kParamGlobalBendCV,
    kParamGlobalFoldCV,
    kParamGlobalVCACV,

    kNumParams
};

// Helper: parameter index for oscillator N (0-based), offset within block
static inline int oscParam( int osc, int offset ) { return kParamOsc1Enable + osc * 3 + offset; }
enum { kOscEnable = 0, kOscTranspose = 1, kOscShape = 2 };

// --- Enum strings ---

static const char* offOnStrings[] = { "Off", "On", NULL };
static const char* oversamplingStrings[] = { "Off", "2x", NULL };
static const char* versionStrings[] = { MELD_VERSION, NULL };

// --- Parameter definitions ---

#define OSC_PARAMS_ON(n) \
    { "Osc" #n " Enable",    0,   1,    1, kNT_unitEnum,       0, offOnStrings }, \
    { "Osc" #n " Transpose", -48, 48,   0, kNT_unitSemitones,  0, NULL }, \
    { "Osc" #n " Shape",     0, 1000, 667, kNT_unitHasStrings, kNT_scaling10, NULL },

#define OSC_PARAMS_OFF(n) \
    { "Osc" #n " Enable",    0,   1,    0, kNT_unitEnum,       0, offOnStrings }, \
    { "Osc" #n " Transpose", -48, 48,   0, kNT_unitSemitones,  0, NULL }, \
    { "Osc" #n " Shape",     0, 1000, 667, kNT_unitHasStrings, kNT_scaling10, NULL },

static _NT_parameter parameters[] = {
    // I/O
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output", 1, 13 )

    // Global
    { "Drift",          0, 100,   25, kNT_unitPercent,    0, NULL },
    { "Drift Rate",     1, 200,   10, kNT_unitHasStrings, kNT_scaling100, NULL },
    { "Fine Tune",   -100, 100,    0, kNT_unitCents,      0, NULL },
    { "Oversampling",   0,   1,    0, kNT_unitEnum,       0, oversamplingStrings },
    { "Global Bend",    0, 1000,   0, kNT_unitPercent,    kNT_scaling10, NULL },
    { "Global Fold",    0, 1000,   0, kNT_unitPercent,    kNT_scaling10, NULL },
    { "MIDI Channel",   1,  16,    1, kNT_unitNone,       0, NULL },
    { "Global VCA",     0, 100,  100, kNT_unitPercent,    0, NULL },

    // Version (read-only)
    { "Version",        0,   0,    0, kNT_unitEnum,       0, versionStrings },

    // Oscillators
    OSC_PARAMS_ON(1) OSC_PARAMS_ON(2) OSC_PARAMS_ON(3)
    OSC_PARAMS_OFF(4) OSC_PARAMS_OFF(5) OSC_PARAMS_OFF(6)
    OSC_PARAMS_OFF(7) OSC_PARAMS_OFF(8)

    // CV Inputs
    NT_PARAMETER_CV_INPUT( "V/OCT CV",       0, 0 )
    NT_PARAMETER_CV_INPUT( "Drift CV",       0, 0 )
    NT_PARAMETER_CV_INPUT( "Global Bend CV", 0, 0 )
    NT_PARAMETER_CV_INPUT( "Global Fold CV", 0, 0 )
    NT_PARAMETER_CV_INPUT( "Global VCA CV",  0, 0 )
};

// --- Parameter pages ---

static const uint8_t pageIO[] = { kParamOutput, kParamOutputMode };
static const uint8_t pageGlobal[] = {
    kParamDrift, kParamDriftRate, kParamFineTune, kParamOversampling,
    kParamGlobalBend, kParamGlobalFold, kParamGlobalVCA, kParamVersion
};
static const uint8_t pageMIDI[] = { kParamMidiChannel };

#define OSC_PAGE(n) \
    static const uint8_t pageOsc##n[] = { \
        kParamOsc##n##Enable, kParamOsc##n##Transpose, kParamOsc##n##Shape \
    };
OSC_PAGE(1) OSC_PAGE(2) OSC_PAGE(3) OSC_PAGE(4)
OSC_PAGE(5) OSC_PAGE(6) OSC_PAGE(7) OSC_PAGE(8)

static const uint8_t pageCV[] = {
    kParamVOctCV, kParamDriftCV, kParamGlobalBendCV, kParamGlobalFoldCV, kParamGlobalVCACV
};

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

// CC14-45 -> 32 value parameters (excludes CV bus selectors and version)
// 8 global + 24 per-osc (3x8)
static const int8_t ccToParam[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,                     // 0-13
    kParamDrift, kParamDriftRate, kParamFineTune, kParamOversampling, // 14-17
    kParamGlobalBend, kParamGlobalFold, kParamMidiChannel, kParamGlobalVCA, // 18-21
    kParamOsc1Enable, kParamOsc1Transpose, kParamOsc1Shape,          // 22-24
    kParamOsc2Enable, kParamOsc2Transpose, kParamOsc2Shape,          // 25-27
    kParamOsc3Enable, kParamOsc3Transpose, kParamOsc3Shape,          // 28-30
    kParamOsc4Enable, kParamOsc4Transpose, kParamOsc4Shape,          // 31-33
    kParamOsc5Enable, kParamOsc5Transpose, kParamOsc5Shape,          // 34-36
    kParamOsc6Enable, kParamOsc6Transpose, kParamOsc6Shape,          // 37-39
    kParamOsc7Enable, kParamOsc7Transpose, kParamOsc7Shape,          // 40-42
    kParamOsc8Enable, kParamOsc8Transpose, kParamOsc8Shape,          // 43-45
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,               // 46-61
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,               // 62-77
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,               // 78-93
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,               // 94-109
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,               // 110-125
    -1,-1                                                            // 126-127
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
    req.sram = sizeof( _meldAlgorithm );
    req.dram = 0;
    req.dtc = 0;
    req.itc = 0;
}

static _NT_algorithm* construct(
    const _NT_algorithmMemoryPtrs& ptrs,
    const _NT_algorithmRequirements& req,
    const int32_t* specifications )
{
    _meldAlgorithm* alg = new ( ptrs.sram ) _meldAlgorithm();
    alg->parameters = parameters;
    alg->parameterPages = &parameterPages;
    return alg;
}

// --- Parameter changed ---

static void parameterChanged( _NT_algorithm* self, int parameter )
{
    _meldAlgorithm* p = (_meldAlgorithm*)self;

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
                p->oscShape[osc] = (float)p->v[parameter] * 0.001f;
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
    case kParamDriftRate:
        p->driftRateHz = (float)p->v[parameter] * 0.01f;
        break;
    case kParamFineTune:
        p->fineTune = exp2f( (float)p->v[parameter] / 1200.0f );
        break;
    case kParamOversampling:
        p->oversample = p->v[parameter];
        break;
    case kParamGlobalBend:
        p->globalBend = (float)p->v[parameter] * 0.001f;
        break;
    case kParamGlobalFold:
        p->globalFold = (float)p->v[parameter] * 0.001f;
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

// sqrt(N) normalization LUT: 1/sqrt(1)..1/sqrt(8)
static const float sqrtNormLUT[9] = {
    0.0f,                                          // 0 active
    1.0f,                                          // 1
    1.0f / 1.41421356f,                            // 2
    1.0f / 1.73205081f,                            // 3
    0.5f,                                          // 4
    1.0f / 2.23606798f,                            // 5
    1.0f / 2.44948975f,                            // 6
    1.0f / 2.64575131f,                            // 7
    1.0f / 2.82842712f,                            // 8
};

static void step(
    _NT_algorithm* self,
    float* busFrames,
    int numFramesBy4 )
{
    _meldAlgorithm* p = (_meldAlgorithm*)self;
    int numFrames = numFramesBy4 * 4;

    float* out = busFrames + ( p->v[kParamOutput] - 1 ) * numFrames;
    bool replace = p->v[kParamOutputMode];

    float sampleRate = (float)NT_globals.sampleRate;
    int actualOversample = p->oversample ? 2 : 1;
    float effectiveSampleRate = sampleRate * (float)actualOversample;

    // Drift filter coefficient: based on drift rate, at base sample rate (not oversampled)
    float driftAlpha = meld::TWO_PI * p->driftRateHz / sampleRate;
    float driftScale = 0.82f / sqrtf( driftAlpha );

    // Precompute bend drive and normalization
    float bendDrive = 1.0f + p->globalBend * 8.0f;
    float bendNorm = 1.0f / meld::soft_clip( bendDrive );

    // Read CV buses (0 = not connected)
    const float* cvVOct = p->v[kParamVOctCV]
        ? busFrames + ( p->v[kParamVOctCV] - 1 ) * numFrames : NULL;
    const float* cvDrift = p->v[kParamDriftCV]
        ? busFrames + ( p->v[kParamDriftCV] - 1 ) * numFrames : NULL;
    const float* cvGlobalBend = p->v[kParamGlobalBendCV]
        ? busFrames + ( p->v[kParamGlobalBendCV] - 1 ) * numFrames : NULL;
    const float* cvGlobalFold = p->v[kParamGlobalFoldCV]
        ? busFrames + ( p->v[kParamGlobalFoldCV] - 1 ) * numFrames : NULL;
    const float* cvGlobalVCA = p->v[kParamGlobalVCACV]
        ? busFrames + ( p->v[kParamGlobalVCACV] - 1 ) * numFrames : NULL;

    // Count active voices for sqrt(N) normalization
    int numActive = 0;
    for ( int v = 0; v < NUM_VOICES; ++v )
        if ( p->oscEnabled[v] ) numActive++;

    float normFactor = sqrtNormLUT[numActive];

    for ( int i = 0; i < numFrames; ++i )
    {
        // Base frequency: MIDI overrides V/OCT when gate is on
        float baseFreq = p->baseFrequency;
        if ( cvVOct && !p->midiGate )
            baseFreq = meld::voct_to_freq( cvVOct[i] );

        float freq = baseFreq * p->pitchBendFactor * p->fineTune;

        // Drift amount with CV modulation
        float driftAmt = p->driftAmount;
        if ( cvDrift )
            driftAmt = fminf( 1.0f, fmaxf( 0.0f, driftAmt + cvDrift[i] * 0.2f ) );

        // Bend amount with CV modulation
        float bendAmt = p->globalBend;
        if ( cvGlobalBend )
            bendAmt = fminf( 1.0f, fmaxf( 0.0f, bendAmt + cvGlobalBend[i] * 0.2f ) );

        // Fold amount with CV modulation
        float foldAmt = p->globalFold;
        if ( cvGlobalFold )
            foldAmt = fminf( 1.0f, fmaxf( 0.0f, foldAmt + cvGlobalFold[i] * 0.2f ) );

        // Recompute bend drive/norm if CV is modulating
        float curBendDrive = bendDrive;
        float curBendNorm = bendNorm;
        if ( cvGlobalBend )
        {
            curBendDrive = 1.0f + bendAmt * 8.0f;
            curBendNorm = 1.0f / meld::soft_clip( curBendDrive );
        }

        // Global VCA with CV
        float vca = p->globalVCA;
        if ( cvGlobalVCA )
            vca = fminf( 1.0f, fmaxf( 0.0f, vca + cvGlobalVCA[i] * 0.2f ) );

        // Advance all 8 drifts once per output sample (at base rate, not oversampled)
        float driftVal[NUM_VOICES];
        for ( int v = 0; v < NUM_VOICES; ++v )
            driftVal[v] = p->drift[v].process( driftAlpha, driftScale );

        // Oversampling loop
        for ( int os = 0; os < actualOversample; ++os )
        {
            float mix = 0.0f;

            for ( int v = 0; v < NUM_VOICES; ++v )
            {
                if ( !p->oscEnabled[v] ) continue;

                // Compute drift pitch deviation (up to +/-50 cents at max drift)
                float driftCents = driftVal[v] * driftAmt * 50.0f;
                float driftMult = meld::cents_to_mult_fast( driftCents );

                // Voice frequency with transpose and drift
                float voiceFreq = freq * p->oscTransposeMult[v] * driftMult;
                if ( voiceFreq < 0.0f ) voiceFreq = 0.0f;

                float inc = voiceFreq / effectiveSampleRate;

                // Advance phase (free-running, never reset)
                meld::phase_advance( p->phase[v], inc );

                // Generate waveform with PolyBLEP anti-aliasing
                float sample;
                if ( p->oscShape[v] > 0.0f )
                    sample = meld::wave_warp_blep( p->phase[v], p->oscShape[v], inc );
                else
                    sample = meld::oscillator_sine( p->phase[v] );

                mix += sample;
            }

            // sqrt(N) normalization
            mix *= normFactor;

            // Post-mix bend (drive/saturation)
            if ( bendAmt > 0.0f )
                mix = meld::soft_clip( mix * curBendDrive ) * curBendNorm;

            // Post-mix fold (wave folding)
            if ( foldAmt > 0.0f )
            {
                float folded = meld::fold_symmetric( mix );
                mix += foldAmt * ( folded - mix );
            }

            // Apply VCA
            mix *= vca;

            // Downsample handling
            if ( actualOversample == 1 )
            {
                // No oversampling: output directly
                mix = p->dcBlocker.process( mix );
                if ( replace )
                    out[i] = mix;
                else
                    out[i] += mix;
            }
            else
            {
                if ( os == 0 )
                {
                    p->dsBuffer = mix;
                }
                else
                {
                    mix = meld::downsample_2x( p->dsBuffer, mix );
                    mix = p->dcBlocker.process( mix );
                    if ( replace )
                        out[i] = mix;
                    else
                        out[i] += mix;
                }
            }
        }
    }
}

// --- MIDI ---

static void midiMessage(
    _NT_algorithm* self,
    uint8_t byte0,
    uint8_t byte1,
    uint8_t byte2 )
{
    _meldAlgorithm* p = (_meldAlgorithm*)self;

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
            p->baseFrequency = meld::midi_note_to_freq( byte1 );
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

// --- Parameter string display ---

// Provides waveform labels for Shape parameters (range 0-1000):
// 0 = "Sine", 333 = "Triangle", 667 = "Saw", 1000 = "Pulse"
// Intermediate values show the morph region and percentage.
static int parameterString( _NT_algorithm* self, int p, int v, char* buff )
{
    // Drift Rate: display 1-200 as "0.01 Hz" to "2.00 Hz"
    if ( p == kParamDriftRate )
    {
        float hz = (float)v * 0.01f;
        int len = NT_floatToString( buff, hz, 2 );
        const char* suffix = " Hz";
        while ( *suffix ) buff[len++] = *suffix++;
        buff[len] = '\0';
        return len;
    }

    for ( int osc = 0; osc < NUM_VOICES; ++osc )
    {
        if ( p == oscParam( osc, kOscShape ) )
        {
            const char* label;
            bool pure = false;

            if ( v == 0 )        { label = "Sine";     pure = true; }
            else if ( v < 333 )  { label = "Sin>Tri";  }
            else if ( v == 333 ) { label = "Triangle";  pure = true; }
            else if ( v < 667 )  { label = "Tri>Saw";  }
            else if ( v == 667 ) { label = "Saw";       pure = true; }
            else if ( v < 1000 ) { label = "Saw>Pls";  }
            else                 { label = "Pulse";     pure = true; }

            // Copy label
            int len = 0;
            const char* s = label;
            while ( *s ) buff[len++] = *s++;

            // Append percentage for intermediate positions
            if ( !pure )
            {
                buff[len++] = ' ';
                float pct = (float)v * 0.1f;
                len += NT_floatToString( buff + len, pct, 1 );
            }

            buff[len] = '\0';
            return len;
        }
    }
    return 0;
}

// --- Factory ---

static const _NT_factory factory = {
    .guid = NT_MULTICHAR('M', 'e', 'l', 'd'),
    .name = "Meld",
    .description = "Meld v" MELD_VERSION " - 8-voice drift oscillator",
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
    .parameterString = parameterString,
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
