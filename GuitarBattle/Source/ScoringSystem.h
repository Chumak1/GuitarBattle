#pragma once
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

    static DifficultySettings getDifficultySettings(Difficulty difficulty)
    {
        DifficultySettings settings;

        switch (difficulty)
        {
        case Difficulty::Easy:
            settings.difficultyMultiplier = 0.80;
            break;

        case Difficulty::Normal:
            settings.difficultyMultiplier = 1.00;
            break;

        case Difficulty::Hard:
            settings.difficultyMultiplier = 1.25;
            break;
        }

        return settings;
    }

    static double getBpmMultiplier(double bpmPercent)
    {
        const double points[][2] =
        {
            { 50.0, 0.50 },
            { 60.0, 0.65 },
            { 70.0, 0.80 },
            { 80.0, 0.90 },
            { 90.0, 0.95 },
            { 100.0, 1.00 },
            { 110.0, 1.03 },
            { 120.0, 1.05 }
        };

        const int count = static_cast<int>(
            sizeof(points) / sizeof(points[0]));

        if (bpmPercent <= points[0][0])
            return points[0][1];

        if (bpmPercent >= points[count - 1][0])
            return points[count - 1][1];

        for (int i = 0; i < count - 1; ++i)
        {
            const double x1 = points[i][0];
            const double y1 = points[i][1];

            const double x2 = points[i + 1][0];
            const double y2 = points[i + 1][1];

            if (bpmPercent >= x1 && bpmPercent <= x2)
            {
                const double t =
                    (bpmPercent - x1) / (x2 - x1);

                return y1 + (y2 - y1) * t;
            }
        }

        return 1.0;
    }

    static double getPitchMultiplier(double pitchErrorCents)
    {
        const double error = std::abs(pitchErrorCents);

        if (error <= 10.0)
            return 1.00;

        if (error <= 25.0)
        {
            const double t = (error - 10.0) / 15.0;
            return 1.00 + (0.90 - 1.00) * t;
        }

        if (error <= 50.0)
        {
            const double t = (error - 25.0) / 25.0;
            return 0.90 + (0.70 - 0.90) * t;
        }

        return 0.0;
    }

    static double getTimingMultiplier(double timingErrorMs)
    {
        const double error = std::abs(timingErrorMs);

        const double points[][2] =
        {
            {   0.0, 1.00 },
            {  10.0, 0.95 },
            {  25.0, 0.85 },
            {  50.0, 0.65 },
            {  75.0, 0.40 },
            { 100.0, 0.20 },
            { 150.0, 0.00 }
        };

        const int count = static_cast<int>(
            sizeof(points) / sizeof(points[0]));

        if (error >= points[count - 1][0])
            return 0.0;

        for (int i = 0; i < count - 1; ++i)
        {
            const double x1 = points[i][0];
            const double y1 = points[i][1];

            const double x2 = points[i + 1][0];
            const double y2 = points[i + 1][1];

            if (error >= x1 && error <= x2)
            {
                const double t =
                    (error - x1) / (x2 - x1);

                return y1 + (y2 - y1) * t;
            }
        }

        return 0.0;
    }

    static HitQuality getHitQuality(
        double pitchErrorCents,
        double timingErrorMs)
    {
        const double pitchError = std::abs(pitchErrorCents);
        const double timingError = std::abs(timingErrorMs);

        if (pitchError > 50.0 || timingError >= 150.0)
            return HitQuality::Miss;

        if (pitchError <= 10.0 && timingError <= 10.0)
            return HitQuality::Perfect;

        if (pitchError <= 25.0 && timingError <= 50.0)
            return HitQuality::Great;

        return HitQuality::Good;
    }

    static NoteResult calculateNoteScore(
        double pitchErrorCents,
        double timingErrorMs)
    {
        NoteResult result;

        result.pitchErrorCents = pitchErrorCents;
        result.timingErrorMs = timingErrorMs;

        result.pitchMultiplier =
            getPitchMultiplier(pitchErrorCents);

        result.timingMultiplier =
            getTimingMultiplier(timingErrorMs);

        result.quality =
            getHitQuality(
                pitchErrorCents,
                timingErrorMs);

        result.noteScore =
            baseNoteScore
            * result.pitchMultiplier
            * result.timingMultiplier;

        if (result.quality == HitQuality::Miss)
            result.noteScore = 0.0;

        return result;
    }

    static SongResult calculateSongResult(
        const std::vector<NoteResult>& notes,
        Difficulty difficulty,
        double bpmPercent)
    {
        SongResult result;

        const auto difficultySettings =
            getDifficultySettings(difficulty);

        result.difficultyMultiplier =
            difficultySettings.difficultyMultiplier;

        result.bpmMultiplier =
            getBpmMultiplier(bpmPercent);

        result.notesPlayed =
            static_cast<int>(notes.size());

        result.notesTotal =
            static_cast<int>(notes.size());

        double totalTimingError = 0.0;
        double totalPitchMultiplier = 0.0;
        double totalTimingMultiplier = 0.0;

        for (const auto& note : notes)
        {
            result.rawScore += note.noteScore;

            totalTimingError +=
                std::abs(note.timingErrorMs);

            totalPitchMultiplier +=
                note.pitchMultiplier;

            totalTimingMultiplier +=
                note.timingMultiplier;

            switch (note.quality)
            {
            case HitQuality::Perfect:
                ++result.perfectCount;
                break;

            case HitQuality::Great:
                ++result.greatCount;
                break;

            case HitQuality::Good:
                ++result.goodCount;
                break;

            case HitQuality::Miss:
                ++result.missCount;
                break;
            }
        }

        result.finalScore =
            result.rawScore
            * result.difficultyMultiplier
            * result.bpmMultiplier;

        if (!notes.empty())
        {
            result.pitchAccuracy =
                totalPitchMultiplier
                / static_cast<double>(notes.size());

            result.timingAccuracy =
                totalTimingMultiplier
                / static_cast<double>(notes.size());

            result.accuracy =
                result.rawScore
                / (baseNoteScore * notes.size());

            result.averageTimingErrorMs =
                totalTimingError
                / static_cast<double>(notes.size());
        }

        return result;
    }
};