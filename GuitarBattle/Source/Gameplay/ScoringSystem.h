#pragma once


#include <JuceHeader.h>
#include <vector>
#include <cmath>
#include <algorithm>

enum class Difficulty
{
    Easy,
    Normal,
    Hard
};

enum class HitQuality
{
    Perfect,
    Great,
    Good,
    Miss
};

struct DifficultySettings
{
    double difficultyMultiplier = 1.0;

    double pitchPerfectCents = 10.0;
    double pitchGoodCents = 25.0;
    double pitchMaxCents = 50.0;
};

struct SpeedSettings
{
    double bpmPercent = 100.0;
    double bpmMultiplier = 1.0;
};

struct NoteResult
{
    double pitchErrorCents = 0.0;
    double timingErrorMs = 0.0;

    double pitchMultiplier = 0.0;
    double timingMultiplier = 0.0;

    double noteScore = 0.0;

    HitQuality quality = HitQuality::Miss;
};


struct FinalNoteResult
{
    NoteResult note;

    double difficultyMultiplier = 1.0;
    double bpmMultiplier = 1.0;

    int score = 0;
};

struct SongResult
{
    double rawScore = 0.0;

    double difficultyMultiplier = 1.0;
    double bpmMultiplier = 1.0;

    double finalScore = 0.0;

    double accuracy = 0.0;
    double pitchAccuracy = 0.0;
    double timingAccuracy = 0.0;

    int notesPlayed = 0;
    int notesTotal = 0;

    int perfectCount = 0;
    int greatCount = 0;
    int goodCount = 0;
    int missCount = 0;

    double averageTimingErrorMs = 0.0;
};


inline double noteNameToFrequency(const juce::String& noteName)
{
    static const juce::String noteNames[12] =
    {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };

    if (noteName.length() < 2)
        return 0.0;

    const auto notePart = noteName.dropLastCharacters(1);
    const int octave = noteName.getTrailingIntValue();

    int noteIndex = -1;

    for (int i = 0; i < 12; ++i)
    {
        if (notePart == noteNames[i])
        {
            noteIndex = i;
            break;
        }
    }

    if (noteIndex < 0)
        return 0.0;

    const int midiNote =
        (octave + 1) * 12 + noteIndex;

    return 440.0 *
        std::pow(
            2.0,
            (static_cast<double>(midiNote) - 69.0) / 12.0);
}



class ScoringSystem
{
public:
    static constexpr double baseNoteScore = 100.0;

    static DifficultySettings getDifficultySettings(Difficulty difficulty);

    static double getBpmMultiplier(double bpmPercent);

    static double getPitchMultiplier(double pitchErrorCents);

    // The reference timing window is +/-150 ms. Easier charts and slower
    // playback widen it: the returned scale multiplies both the hit window and
    // the scoring thresholds, so a beginner is not punished for the same
    // millisecond error on a level-1 chart at 70% speed.
    static double getTimingWindowScale(int songLevel, double bpmPercent);

    // Pitch window in cents for the hit decision and the score curve. The
    // reference is 50 cents; easier charts accept a slightly detuned note
    // instead of turning it into a miss.
    static double getPitchWindowCents(int songLevel);

    static double getTimingMultiplier(double timingErrorMs, double windowScale = 1.0);

    static HitQuality getHitQuality(double pitchErrorCents, double timingErrorMs, double windowScale = 1.0);

    static NoteResult calculateNoteScore(double pitchErrorCents, double timingErrorMs, double windowScale = 1.0);

    // Points multiplier for the song's published 1-10 difficulty. Harder charts
    // are worth more per note, so the catalogue is not a flat score list.
    static double getSongLevelMultiplier(int songLevel);

    static FinalNoteResult calculateFinalNoteScore(double pitchErrorCents, double timingErrorMs, Difficulty difficulty, double bpmPercent, int songLevel = 5);



    static SongResult calculateSongResult(const std::vector<NoteResult>& notes, Difficulty difficulty, double bpmPercent, int songLevel = 5);
};