#include "GameplayEngine.h"

#include <cmath>

GameplayEngine::NoteEvaluation GameplayEngine::evaluateNote(
    int targetMidi,
    int detectedMidi,
    double detectedFrequency,
    double timingErrorMs,
    Difficulty difficulty,
    double speedPercent,
    int songLevel)
{
    NoteEvaluation result;
    result.correctPitch = detectedMidi == targetMidi;

    // The same scale that widens the score curve widens the hard hit window,
    // so a note that is inside the window is never scored as a miss.
    const double windowScale =
        ScoringSystem::getTimingWindowScale(songLevel, speedPercent);
    result.insideTimingWindow = std::abs(timingErrorMs) < 150.0 * windowScale;

    const double targetFrequency =
        440.0 * std::pow(2.0, (static_cast<double>(targetMidi) - 69.0) / 12.0);

    if (targetFrequency > 0.0 && detectedFrequency > 0.0)
    {
        result.pitchErrorCents =
            1200.0 * std::log2(detectedFrequency / targetFrequency);
    }

    result.scoringResult = ScoringSystem::calculateFinalNoteScore(
        result.pitchErrorCents, timingErrorMs, difficulty, speedPercent, songLevel);

    return result;
}
