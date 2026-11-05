#include "ScoringSystem.h"

#include <cmath>
#include <algorithm>

DifficultySettings ScoringSystem::getDifficultySettings(Difficulty difficulty)
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

double ScoringSystem::getBpmMultiplier(double bpmPercent)
{
    const double points[][2] =
    {
        { 50.0, 0.50 },
        { 60.0, 0.65 },
        { 70.0, 0.80 },
        { 75.0, 0.85 },
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

double ScoringSystem::getPitchMultiplier(double pitchErrorCents)
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

double ScoringSystem::getTimingWindowScale(int songLevel, double bpmPercent)
{
    // Levels 5-10 keep the reference +/-150 ms window; every level below adds
    // 20%, so a level-1 chart is 1.8x as forgiving (270 ms). Slower playback
    // adds the square-root part of the slowdown (50% -> +41%): the window
    // follows the musical grid without turning half speed into a 300 ms window
    // on its own. The cap keeps the easiest chart at half speed at +/-300 ms.
    const int level = juce::jlimit(1, 10, songLevel);

    const double levelBonus = level < 5
        ? static_cast<double>(5 - level) * 0.20
        : 0.0;

    const double speed = juce::jlimit(50.0, 120.0, bpmPercent);

    const double speedBonus = speed < 100.0
        ? std::sqrt(100.0 / speed) - 1.0
        : 0.0;

    return juce::jlimit(1.0, 2.0, 1.0 + levelBonus + speedBonus);
}

double ScoringSystem::getPitchWindowCents(int songLevel)
{
    // Levels 5-10 keep the reference 50 cents; every level below adds 8, so
    // the easiest chart accepts up to 82 cents. That covers a slightly
    // detuned acoustic string without ever reaching a whole semitone.
    const int level = juce::jlimit(1, 10, songLevel);

    return level < 5
        ? 50.0 + static_cast<double>(5 - level) * 8.0
        : 50.0;
}

double ScoringSystem::getTimingMultiplier(double timingErrorMs, double windowScale)
{
    const double scale = windowScale > 0.0 ? windowScale : 1.0;
    const double error = std::abs(timingErrorMs) / scale;

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

HitQuality ScoringSystem::getHitQuality( double pitchErrorCents, double timingErrorMs, double windowScale)
{
    const double pitchError = std::abs(pitchErrorCents);
    const double scale = windowScale > 0.0 ? windowScale : 1.0;
    const double timingError = std::abs(timingErrorMs) / scale;

    if (pitchError > 50.0 || timingError >= 150.0)
        return HitQuality::Miss;

    if (pitchError <= 10.0 && timingError <= 10.0)
        return HitQuality::Perfect;

    if (pitchError <= 25.0 && timingError <= 50.0)
        return HitQuality::Great;

    return HitQuality::Good;

}

NoteResult ScoringSystem::calculateNoteScore( double pitchErrorCents, double timingErrorMs, double windowScale)
{
    NoteResult result;

    result.pitchErrorCents = pitchErrorCents;
    result.timingErrorMs = timingErrorMs;

    result.pitchMultiplier =
        getPitchMultiplier(pitchErrorCents);

    result.timingMultiplier =
        getTimingMultiplier(timingErrorMs, windowScale);

    result.quality =
        getHitQuality(
            pitchErrorCents,
            timingErrorMs,
            windowScale);

    result.noteScore =
        baseNoteScore
        * result.pitchMultiplier
        * result.timingMultiplier;

    if (result.quality == HitQuality::Miss)
        result.noteScore = 0.0;

    return result;

}

double ScoringSystem::getSongLevelMultiplier(int songLevel)
{
    // Published difficulty 1-10 -> points multiplier. The curve starts below
    // 1.0 for the easiest charts and tops out at 1.8, so a level-10 chart is
    // worth about 2.4x a level-1 one: hard songs pay off without turning the
    // leaderboard into a difficulty contest.
    static const double multipliers[] =
    {
        0.75, 0.85, 0.95, 1.00, 1.10, 1.22, 1.35, 1.50, 1.65, 1.80
    };

    const int index = juce::jlimit(1, 10, songLevel) - 1;
    return multipliers[index];
}

FinalNoteResult ScoringSystem::calculateFinalNoteScore( double pitchErrorCents, double timingErrorMs, Difficulty difficulty, double bpmPercent, int songLevel)
{
    FinalNoteResult result;

    // Easy levels tolerate a detuned note: the pitch error is measured against
    // the widened window, so the score curve and the hit quality match the hit
    // decision in GameplayEngine::evaluateNote.
    const double pitchWindow = getPitchWindowCents(songLevel);
    const double scaledPitchError =
        pitchWindow > 0.0 ? pitchErrorCents * (50.0 / pitchWindow)
                          : pitchErrorCents;

    result.note =
        calculateNoteScore(
            scaledPitchError,
            timingErrorMs,
            getTimingWindowScale(songLevel, bpmPercent));

    // Keep the real error for the stats and the UI; only the evaluation used
    // the widened window.
    result.note.pitchErrorCents = pitchErrorCents;

    const auto difficultySettings =
        getDifficultySettings(
            difficulty);

    result.difficultyMultiplier =
        difficultySettings.difficultyMultiplier;

    result.bpmMultiplier =
        getBpmMultiplier(
            bpmPercent);

    result.score =
        static_cast<int>(
            std::round(
                result.note.noteScore
                * result.difficultyMultiplier
                * result.bpmMultiplier
                * getSongLevelMultiplier(songLevel)));

    return result;

}

SongResult ScoringSystem::calculateSongResult( const std::vector<NoteResult>& notes, Difficulty difficulty, double bpmPercent, int songLevel)
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
        * result.bpmMultiplier
        * getSongLevelMultiplier(songLevel);

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
