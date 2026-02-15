# Meld

An 8-voice drift oscillator for the [Expert Sleepers Disting NT](https://expert-sleepers.co.uk/distingNT.html).

Meld stacks up to eight oscillators with independent pitch drift, waveform morphing, saturation, and wave folding. Use it for thick unison leads, shimmering pads, detuned drone textures, or anything that benefits from many voices slowly wandering apart.

## Installation

1. Download or build `meld.o` and `plugin.json` from the `plugins/` directory.
2. Copy both files into a folder on your Disting NT SD card under `plugins/` (e.g. `plugins/meld/`).
3. Power on the Disting NT — Meld will appear in the algorithm list under **Instruments**.

## Parameters

Parameters are organized into pages on the Disting NT display.

### I/O

| Parameter   | Range | Default | Description |
|-------------|-------|---------|-------------|
| Output      | 1–13  | 1       | Audio output bus |
| Output Mode | Replace / Mix | Replace | Replace the bus signal or mix into it |

### Global

| Parameter    | Range         | Default | Description |
|--------------|---------------|---------|-------------|
| Drift        | 0–100%        | 25%     | Amount of random pitch deviation per voice (up to ±50 cents at 100%) |
| Drift Rate   | 0.01–2.00 Hz  | 0.10 Hz | Speed of the pitch drift — lower values give slow, gentle movement |
| Fine Tune    | -100 to +100 cents | 0  | Global fine-tuning offset applied to all voices |
| Oversampling | Off / 2x      | Off     | 2x oversampling reduces aliasing at the cost of more CPU |
| Global Bend  | 0–100%        | 0%      | Post-mix soft saturation — adds harmonic warmth and compression |
| Global Fold  | 0–100%        | 0%      | Post-mix wave folding — adds metallic, bell-like overtones |
| Global VCA   | 0–100%        | 100%    | Master output level |
| Version      | —             | —       | Displays the current firmware version (read-only) |

### MIDI

| Parameter    | Range | Default | Description |
|--------------|-------|---------|-------------|
| MIDI Channel | 1–16  | 1       | Which MIDI channel Meld responds to |

### Oscillators 1–8

Each oscillator has three parameters. Oscillators 1–3 default to **On**; 4–8 default to **Off**.

| Parameter | Range              | Default  | Description |
|-----------|--------------------|----------|-------------|
| Enable    | Off / On           | On (1–3) | Activate or deactivate this voice |
| Transpose | -48 to +48 semitones | 0      | Pitch offset relative to the base note |
| Shape     | 0–1000             | 667 (Saw) | Waveform morph position (see below) |

### CV Inputs

Each CV input can be assigned to any bus on the Disting NT (0 = disconnected).

| Input          | Effect |
|----------------|--------|
| V/OCT CV       | Pitch control (1V/octave, 0V = C4). Overridden by MIDI when a note is held. |
| Drift CV       | Modulates drift amount (±20% of range per volt) |
| Global Bend CV | Modulates bend amount (±20% of range per volt) |
| Global Fold CV | Modulates fold amount (±20% of range per volt) |
| Global VCA CV  | Modulates VCA level (±20% of range per volt) |

## Waveform Shape

The Shape parameter morphs smoothly between four classic waveforms:

```
0          333        667        1000
|           |           |           |
Sine ——— Triangle ——— Saw ——— Pulse
```

| Value   | Display    | Waveform |
|---------|------------|----------|
| 0       | Sine       | Pure sine wave |
| 1–332   | Sin>Tri    | Crossfade from sine toward triangle |
| 333     | Triangle   | Pure triangle wave |
| 334–666 | Tri>Saw    | Crossfade from triangle toward saw |
| 667     | Saw        | Pure sawtooth wave |
| 668–999 | Saw>Pls    | Crossfade from saw toward pulse |
| 1000    | Pulse      | Pure square/pulse wave |

Saw and pulse waveforms use PolyBLEP anti-aliasing to reduce digital artifacts.

## Patching Tips

- **Thick unison** — Enable 3–5 oscillators at the same transpose with Drift at 15–30% and a slow Drift Rate. Classic supersaw-style sound.
- **Octave stacks** — Set oscillator transposes to 0, -12, +12, +7 for a rich organ-like timbre.
- **Evolving drones** — Use all 8 voices with high drift (60–80%) and a very slow rate (0.01–0.05 Hz). The voices wander apart over time.
- **Harmonic saturation** — Turn up Global Bend to add soft clipping warmth. Works well at moderate levels (20–40%) to thicken the mix.
- **Metallic textures** — Global Fold adds wave-folding overtones. Combine with CV modulation for dynamic timbral shifts.
- **Shape variety** — Set different Shape values per oscillator (e.g. one sine, one saw, one pulse) for complex layered timbres.

## MIDI Control

Meld responds to MIDI notes, pitch bend, and control change messages on the configured channel.

**Note On/Off** — Sets the base pitch. Oscillators are free-running (no phase reset on note-on), so there are no click artifacts.

**Pitch Bend** — ±2 semitones range.

**Control Change mapping:**

| CC  | Parameter       | CC  | Parameter       |
|-----|-----------------|-----|-----------------|
| 14  | Drift           | 31  | Osc 4 Enable    |
| 15  | Drift Rate      | 32  | Osc 4 Transpose |
| 16  | Fine Tune       | 33  | Osc 4 Shape     |
| 17  | Oversampling    | 34  | Osc 5 Enable    |
| 18  | Global Bend     | 35  | Osc 5 Transpose |
| 19  | Global Fold     | 36  | Osc 5 Shape     |
| 20  | MIDI Channel    | 37  | Osc 6 Enable    |
| 21  | Global VCA      | 38  | Osc 6 Transpose |
| 22  | Osc 1 Enable    | 39  | Osc 6 Shape     |
| 23  | Osc 1 Transpose | 40  | Osc 7 Enable    |
| 24  | Osc 1 Shape     | 41  | Osc 7 Transpose |
| 25  | Osc 2 Enable    | 42  | Osc 7 Shape     |
| 26  | Osc 2 Transpose | 43  | Osc 8 Enable    |
| 27  | Osc 2 Shape     | 44  | Osc 8 Transpose |
| 28  | Osc 3 Enable    | 45  | Osc 8 Shape     |
| 29  | Osc 3 Transpose |     |                 |
| 30  | Osc 3 Shape     |     |                 |

CC values (0–127) are scaled to each parameter's full range.

## Building from Source

Requirements:
- `arm-none-eabi-c++` (ARM GCC toolchain)
- The [Disting NT API](https://github.com/expertsleepersltd/distingNT_API) (included as `distingNT_API/`)

```
make
```

This produces `plugins/meld.o` and `plugins/plugin.json`. Copy both to your SD card.

## License

MIT — see [LICENSE](LICENSE).
