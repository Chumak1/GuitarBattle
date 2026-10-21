#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>

// Shared, genre-aware harmony used by the guitar, bass, rhythm-guitar and piano generators.
// Each style has four distinct progressions. Selection is deterministic from the song seed.
namespace DailyHarmony
{
    struct Chord
    {
        std::string name;
        int rootMidi;
        std::array<int, 3> tonesPc;
        std::array<int, 3> pianoMidi;
    };

    using Progression = std::array<Chord, 4>;

    enum Quality { Minor, Major, Diminished, Dominant7, Minor7, Major7, Power };

    inline Chord makeChord(const char* name, int rootPc, Quality quality)
    {
        (void)name; // Names are rebuilt from the transposed root and chord quality.
        static constexpr int intervals[7][3] = {
            {0,3,7},   // minor
            {0,4,7},   // major
            {0,3,6},   // diminished
            {0,4,10},  // dominant 7
            {0,3,10},  // minor 7
            {0,4,11},  // major 7
            {0,7,7}    // power chord
        };
        static constexpr const char* noteNames[12] = {
            "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
        };
        const int pc = (rootPc % 12 + 12) % 12;
        const int* iv = intervals[static_cast<int>(quality)];
        const int root = 24 + pc; // bass register, C1..B1, matching the reference MIDI
        const char* suffix = "";
        switch (quality)
        {
            case Minor: suffix = "m"; break;
            case Major: suffix = ""; break;
            case Diminished: suffix = "dim"; break;
            case Dominant7: suffix = "7"; break;
            case Minor7: suffix = "m7"; break;
            case Major7: suffix = "maj7"; break;
            case Power: suffix = "5"; break;
        }
        return { std::string(noteNames[pc]) + suffix, root,
            {{pc, (pc + iv[1]) % 12, (pc + iv[2]) % 12}},
            {{60 + pc, 60 + (pc + iv[1]) % 12, 60 + (pc + iv[2]) % 12}} };
    }

    // Style IDs match MidiMelodyGenerator::Style:
    // 0 Vai, 1 Power, 2 Progressive, 3 Blues Rock, 4 Hard Rock, 5 Polyphia,
    // 6 Trivium, 7 Djent, 8 Neoclassical, 9 Classic Rock, 10 Heavy Metal,
    // 11 Symphonic, 12 Technical, 13 Metalcore, 14 Guitar Fusion, 15 Blues,
    // 16 Funk/Soul, 17 Jazz Fusion, 18 Alternative/Indie, 19 Cinematic, 20 Country.
    // The profiles intentionally use genre-appropriate harmonic vocabulary. Not every
    // progression is diatonic: blues dominants, modal bII chords, diminished passing
    // chords and chromatic metal power chords are intentional colour, not random errors.
    inline int seedHash(int seed)
    {
        std::uint32_t x = static_cast<std::uint32_t>(seed);
        x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
        return static_cast<int>(x & 0x7fffffffU);
    }

    // Power Rock has a larger song-level vocabulary: 16 progressions in each
    // of seven practical lead-guitar keys. The seed selects both dimensions.
    inline int powerRockTonicPc(int seed)
    {
        static constexpr int tonics[7] = {4, 6, 7, 9, 11, 0, 2}; // E, F#, G, A, B, C, D
        return tonics[(seedHash(seed) % 112) / 16];
    }

    inline Progression makeProgression(int style, int variant)
    {
        using Q = Quality;
        if (style == 1)
        {
            struct Spec { const char* name; int offset; Q q; };
            static constexpr Spec songs[16][4] = {
                {{"i",0,Minor},{"VI",8,Major},{"iv",5,Minor},{"V7",7,Dominant7}},
                {{"i",0,Minor},{"VII",10,Major},{"VI",8,Major},{"V7",7,Dominant7}},
                {{"i",0,Minor},{"iv",5,Minor},{"VI",8,Major},{"V7",7,Dominant7}},
                {{"i",0,Minor},{"III",3,Major},{"iv",5,Minor},{"V7",7,Dominant7}},
                {{"i",0,Minor},{"VI",8,Major},{"III",3,Major},{"VII",10,Major}},
                {{"i",0,Minor},{"VII",10,Major},{"III",3,Major},{"VI",8,Major}},
                {{"i",0,Minor},{"iv",5,Minor},{"VII",10,Major},{"III",3,Major}},
                {{"i",0,Minor},{"III",3,Major},{"VII",10,Major},{"VI",8,Major}},
                {{"i",0,Minor},{"II",2,Diminished},{"V7",7,Dominant7},{"i",0,Minor}},
                {{"i",0,Minor},{"iv",5,Minor},{"ii-dim",2,Diminished},{"V7",7,Dominant7}},
                {{"VI",8,Major},{"VII",10,Major},{"i",0,Minor},{"V7",7,Dominant7}},
                {{"iv",5,Minor},{"VI",8,Major},{"i",0,Minor},{"V7",7,Dominant7}},
                {{"i",0,Minor},{"III",3,Major},{"VI",8,Major},{"V7",7,Dominant7}},
                {{"i",0,Minor},{"VII",10,Major},{"iv",5,Minor},{"V7",7,Dominant7}},
                {{"i",0,Minor},{"VI",8,Major},{"VII",10,Major},{"iv",5,Minor}},
                {{"i",0,Minor},{"iv",5,Minor},{"V7",7,Dominant7},{"VI",8,Major}}
            };
            const int safeVariant = ((variant % 112) + 112) % 112;
            const int progression = safeVariant % 16;
            // variant encodes progression and key: low four bits select the song,
            // the remaining bits select a tonic. Root offsets are transposed from E.
            static constexpr int tonics[7] = {4, 6, 7, 9, 11, 0, 2};
            const int keyIndex = (safeVariant / 16) % 7;
            const int tonicPc = tonics[keyIndex];
            Progression out{};
            for (int i = 0; i < 4; ++i)
            {
                const auto& spec = songs[progression][i];
                const int rootPc = (tonicPc + spec.offset) % 12;
                out[static_cast<std::size_t>(i)] = makeChord(spec.name, rootPc, spec.q);
            }
            return out;
        }
        struct Spec { const char* name; int pc; Q q; };
        static constexpr Spec profiles[21][4][4] = {
            // 0 Steve Vai / expressive rock: dominant vamps, modal moves, tension.
            {
                {{"E7",4,Dominant7},{"D",2,Major},{"A",9,Major},{"Bm",11,Minor}},
                {{"E",4,Major},{"A",9,Major},{"D",2,Major},{"Bm",11,Minor}},
                {{"E",4,Major},{"F#m",6,Minor},{"A",9,Major},{"D",2,Major}},
                {{"A",9,Major},{"D",2,Major},{"E",4,Major},{"Bm",11,Minor}}
            },
            // 1 Power Rock: four E-minor progressions with distinct section character.
            // The raised D# in B7 supports the harmonic-minor cadence back to Em.
            {
                {{"Em",4,Minor},{"C",0,Major},{"Am",9,Minor},{"B7",11,Dominant7}}, // epic / classic
                {{"Em",4,Minor},{"D",2,Major},{"C",0,Major},{"B7",11,Dominant7}},  // descending drive
                {{"Am",9,Minor},{"C",0,Major},{"Em",4,Minor},{"B7",11,Dominant7}}, // build to cadence
                {{"Em",4,Minor},{"G",7,Major},{"Am",9,Minor},{"B7",11,Dominant7}}  // melodic lift
            },
            // 2 Progressive Metal: Phrygian bII, minor movement and diminished tension.
            {
                {{"Em",4,Minor},{"F",5,Major},{"Dm",2,Minor},{"Bdim",11,Diminished}},
                {{"Em",4,Minor},{"F",5,Major},{"C",0,Major},{"Bdim",11,Diminished}},
                {{"Em",4,Minor},{"Dm",2,Minor},{"F",5,Major},{"Em",4,Minor}},
                {{"Em",4,Minor},{"F",5,Major},{"G",7,Major},{"Bdim",11,Diminished}}
            },
            // 3 Blues Rock: dominant chords and blues turnarounds.
            {
                {{"E7",4,Dominant7},{"A7",9,Dominant7},{"E7",4,Dominant7},{"B7",11,Dominant7}},
                {{"E7",4,Dominant7},{"D7",2,Dominant7},{"A7",9,Dominant7},{"B7",11,Dominant7}},
                {{"E7",4,Dominant7},{"A7",9,Dominant7},{"G7",7,Dominant7},{"E7",4,Dominant7}},
                {{"Em7",4,Minor7},{"A7",9,Dominant7},{"Em7",4,Minor7},{"B7",11,Dominant7}}
            },
            // 4 Hard Rock: riff-friendly major/mixolydian and minor-leaning options.
            {
                {{"A",9,Major},{"D",2,Major},{"E",4,Major},{"A",9,Major}},
                {{"A",9,Major},{"F#m",6,Minor},{"D",2,Major},{"E",4,Major}},
                {{"F#m",6,Minor},{"D",2,Major},{"A",9,Major},{"E",4,Major}},
                {{"A",9,Major},{"C#m",1,Minor},{"D",2,Major},{"E",4,Major}}
            },
            // 5 Polyphia-inspired: bright Lydian/major colours with unexpected movement.
            {
                {{"E",4,Major},{"F#",6,Major},{"C#m",1,Minor},{"B",11,Major}},
                {{"Emaj7",4,Major7},{"F#",6,Major},{"Emaj7",4,Major7},{"B",11,Major}},
                {{"C#m",1,Minor},{"F#",6,Major},{"E",4,Major},{"B",11,Major}},
                {{"E",4,Major},{"F#",6,Major},{"G#m",8,Minor},{"C#m",1,Minor}}
            },
            // 6 Trivium-inspired: heavy minor, bII and diminished turnaround.
            {
                {{"Em",4,Minor},{"F",5,Major},{"Bdim",11,Diminished},{"Em",4,Minor}},
                {{"Em",4,Minor},{"F",5,Major},{"C",0,Major},{"Bdim",11,Diminished}},
                {{"Em",4,Minor},{"F",5,Major},{"Dm",2,Minor},{"Bdim",11,Diminished}},
                {{"Em",4,Minor},{"Dm",2,Minor},{"F",5,Major},{"Em",4,Minor}}
            },
            // 7 Djent / modern progressive: power-chord pedal tones and half-step tension.
            {
                {{"E5",4,Power},{"F5",5,Power},{"G5",7,Power},{"F5",5,Power}},
                {{"E5",4,Power},{"F5",5,Power},{"D5",2,Power},{"C5",0,Power}},
                {{"D5",2,Power},{"F5",5,Power},{"C5",0,Power},{"A5",9,Power}},
                {{"E5",4,Power},{"C5",0,Power},{"F5",5,Power},{"A5",9,Power}}
            },
            // 8 Neoclassical Metal: harmonic minor, diminished passing chord, dominant cadence.
            {
                {{"Em",4,Minor},{"Am",9,Minor},{"F#dim",6,Diminished},{"B7",11,Dominant7}},
                {{"Em",4,Minor},{"C",0,Major},{"Am",9,Minor},{"B7",11,Dominant7}},
                {{"Em",4,Minor},{"F#dim",6,Diminished},{"B7",11,Dominant7},{"Em",4,Minor}},
                {{"Am",9,Minor},{"C",0,Major},{"F#dim",6,Diminished},{"B7",11,Dominant7}}
            },
            // 9 Classic Rock: A major family, not the previous unrelated A-minor loop.
            {
                {{"A",9,Major},{"D",2,Major},{"E",4,Major},{"A",9,Major}},
                {{"A",9,Major},{"F#m",6,Minor},{"D",2,Major},{"E",4,Major}},
                {{"F#m",6,Minor},{"D",2,Major},{"A",9,Major},{"E",4,Major}},
                {{"A",9,Major},{"C#m",1,Minor},{"D",2,Major},{"E",4,Major}}
            },
            // 10 Heavy Metal: dark minor/phrygian, avoid a constant major resolution.
            {
                {{"Em",4,Minor},{"F",5,Major},{"C",0,Major},{"Em",4,Minor}},
                {{"Em",4,Minor},{"F",5,Major},{"Dm",2,Minor},{"Bdim",11,Diminished}},
                {{"Em",4,Minor},{"Dm",2,Minor},{"F",5,Major},{"Em",4,Minor}},
                {{"Em",4,Minor},{"C",0,Major},{"Bdim",11,Diminished},{"Em",4,Minor}}
            },
            // 11 Symphonic Metal: minor foundations with cinematic lift and cadence.
            {
                {{"Em",4,Minor},{"C",0,Major},{"Am",9,Minor},{"B7",11,Dominant7}},
                {{"Em",4,Minor},{"F#dim",6,Diminished},{"C",0,Major},{"B7",11,Dominant7}},
                {{"Am",9,Minor},{"F#dim",6,Diminished},{"B7",11,Dominant7},{"Em",4,Minor}},
                {{"Em",4,Minor},{"C",0,Major},{"F#dim",6,Diminished},{"B7",11,Dominant7}}
            },
            // 12 Technical Metal: riff-first chromatic power chords.
            {
                {{"E5",4,Power},{"F5",5,Power},{"G5",7,Power},{"F5",5,Power}},
                {{"D5",2,Power},{"F5",5,Power},{"C5",0,Power},{"A5",9,Power}},
                {{"E5",4,Power},{"F5",5,Power},{"D5",2,Power},{"C5",0,Power}},
                {{"E5",4,Power},{"C5",0,Power},{"F5",5,Power},{"A5",9,Power}}
            },
            // 13 Modern Metalcore: punchy pedal/riff progressions.
            {
                {{"E5",4,Power},{"F5",5,Power},{"D5",2,Power},{"C5",0,Power}},
                {{"D5",2,Power},{"F5",5,Power},{"C5",0,Power},{"A5",9,Power}},
                {{"E5",4,Power},{"G5",7,Power},{"F5",5,Power},{"C5",0,Power}},
                {{"E5",4,Power},{"F5",5,Power},{"C5",0,Power},{"A5",9,Power}}
            },
            // 14 Guitar Fusion: D Dorian colours, dominant motion and modal interchange.
            {
                {{"Dm7",2,Minor7},{"G7",7,Dominant7},{"Dm7",2,Minor7},{"Am7",9,Minor7}},
                {{"Dm7",2,Minor7},{"G7",7,Dominant7},{"Cmaj7",0,Major7},{"Am7",9,Minor7}},
                {{"Dm7",2,Minor7},{"Em7",4,Minor7},{"G7",7,Dominant7},{"Dm7",2,Minor7}},
                {{"Dm7",2,Minor7},{"Cmaj7",0,Major7},{"G7",7,Dominant7},{"Am7",9,Minor7}}
            },
            // 15 Blues: dominant language remains bluesy, with a minor-blues option.
            {
                {{"E7",4,Dominant7},{"A7",9,Dominant7},{"E7",4,Dominant7},{"B7",11,Dominant7}},
                {{"E7",4,Dominant7},{"D7",2,Dominant7},{"A7",9,Dominant7},{"B7",11,Dominant7}},
                {{"E7",4,Dominant7},{"A7",9,Dominant7},{"E7",4,Dominant7},{"E7",4,Dominant7}},
                {{"Am7",9,Minor7},{"D7",2,Dominant7},{"E7",4,Dominant7},{"B7",11,Dominant7}}
            },
            // 16 Funk / Soul: compact Dorian vamp and syncopation-friendly changes.
            {
                {{"Dm7",2,Minor7},{"G7",7,Dominant7},{"Dm7",2,Minor7},{"G7",7,Dominant7}},
                {{"Dm7",2,Minor7},{"G7",7,Dominant7},{"Cmaj7",0,Major7},{"G7",7,Dominant7}},
                {{"Dm7",2,Minor7},{"Em7",4,Minor7},{"G7",7,Dominant7},{"Dm7",2,Minor7}},
                {{"Dm7",2,Minor7},{"Am7",9,Minor7},{"G7",7,Dominant7},{"Dm7",2,Minor7}}
            },
            // 17 Jazz Fusion: ii-V, modal movement and richer seventh chords.
            {
                {{"Dm7",2,Minor7},{"G7",7,Dominant7},{"Cmaj7",0,Major7},{"Am7",9,Minor7}},
                {{"Dm7",2,Minor7},{"G7",7,Dominant7},{"Em7",4,Minor7},{"Am7",9,Minor7}},
                {{"Dm7",2,Minor7},{"Cmaj7",0,Major7},{"Gm7",7,Minor7},{"Am7",9,Minor7}},
                {{"Dm7",2,Minor7},{"F",5,Major},{"Em7",4,Minor7},{"Am7",9,Minor7}}
            },
            // 18 Alternative / Indie: A Dorian and minor-rock options.
            {
                {{"Am",9,Minor},{"G",7,Major},{"D",2,Major},{"Am",9,Minor}},
                {{"Am",9,Minor},{"G",7,Major},{"D",2,Major},{"Em",4,Minor}},
                {{"Am",9,Minor},{"D",2,Major},{"G",7,Major},{"Em",4,Minor}},
                {{"Am",9,Minor},{"C",0,Major},{"G",7,Major},{"D",2,Major}}
            },
            // 19 Cinematic / Experimental: dark D-minor colours and strong cadences.
            {
                {{"Dm",2,Minor},{"Bb",10,Major},{"Gm",7,Minor},{"A7",9,Dominant7}},
                {{"Dm",2,Minor},{"C#dim",1,Diminished},{"Bb",10,Major},{"A7",9,Dominant7}},
                {{"Dm",2,Minor},{"Gm",7,Minor},{"Bb",10,Major},{"A7",9,Dominant7}},
                {{"Dm",2,Minor},{"Dm",2,Minor},{"Gm",7,Minor},{"A7",9,Dominant7}}
            },
            // 20 Country / Americana: open major-family harmony with clear cadences.
            {
                {{"A",9,Major},{"D",2,Major},{"A",9,Major},{"E",4,Major}},
                {{"A",9,Major},{"E",4,Major},{"F#m",6,Minor},{"D",2,Major}},
                {{"A",9,Major},{"D",2,Major},{"E",4,Major},{"A",9,Major}},
                {{"F#m",6,Minor},{"D",2,Major},{"A",9,Major},{"E",4,Major}}
            }
        };

        const int safeStyle = (style < 0 || style > 20) ? 1 : style;
        const int safeVariant = ((variant % 4) + 4) % 4;
        const auto& p = profiles[static_cast<std::size_t>(safeStyle)][static_cast<std::size_t>(safeVariant)];
        return {{ makeChord(p[0].name,p[0].pc,p[0].q), makeChord(p[1].name,p[1].pc,p[1].q),
                  makeChord(p[2].name,p[2].pc,p[2].q), makeChord(p[3].name,p[3].pc,p[3].q) }};
    }

    inline const Progression& forSeed(int seed, int style)
    {
        static const auto all = [] {
            std::array<std::array<Progression, 4>, 21> data{};
            for (int s = 0; s < 21; ++s)
                for (int v = 0; v < 4; ++v)
                    data[static_cast<std::size_t>(s)][static_cast<std::size_t>(v)] = makeProgression(s, v);
            return data;
        }();

        std::uint32_t x = static_cast<std::uint32_t>(seed);
        x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
        const int safeStyle = (style < 0 || style > 20) ? 1 : style;
        return all[static_cast<std::size_t>(safeStyle)][static_cast<std::size_t>(x & 3U)];
    }

    inline const Progression& forSeed(int seed) { return forSeed(seed, 1); }

    inline int progressionIndex(int seed)
    {
        const std::uint32_t x = static_cast<std::uint32_t>(seedHash(seed));
        // 112 distinct Power Rock combinations: 16 progressions x 7 keys.
        return static_cast<int>(x % 112U);
    }

    inline Progression forSection(int seed, int style, int section)
    {
        if (style == 1)
        {
            const int base = progressionIndex(seed);
            const int keyIndex = base / 16;
            const int songVariant = base % 16;
            // Keep one tonal centre, but let verse/build/chorus/bridge blocks
            // evolve through related minor-key progressions instead of looping
            // the same four chords for the entire song.
            const int sectionVariant = (songVariant + (section * 5)) % 16;
            return makeProgression(style, keyIndex * 16 + sectionVariant);
        }
        return makeProgression(style, seedHash(seed) & 3);
    }

    inline std::string nameForSeed(int seed) { return forSeed(seed)[0].name; }
}
