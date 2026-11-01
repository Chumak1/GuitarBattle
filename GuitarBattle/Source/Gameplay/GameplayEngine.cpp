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
    result.insideTimingWindow = std::abs(timingErrorMs) < 150.0;

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
