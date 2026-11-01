#pragma once

#include "ScoringSystem.h"

// Pure gameplay calculations: no UI, audio callbacks, or session state here.
class GameplayEngine
{
public:
    struct NoteEvaluation
    {
        bool correctPitch = false;
        bool insideTimingWindow = false;
        double pitchErrorCents = 0.0;
        FinalNoteResult scoringResult;
    };

    // Exact MIDI pitch match and the reference +/-150 ms timing window, widened
    // by ScoringSystem::getTimingWindowScale() on easier charts and at reduced
    // BPM. Pitch deviation still determines score quality.
    static NoteEvaluation evaluateNote(
        int targetMidi,
        int detectedMidi,
        double detectedFrequency,
        double timingErrorMs,
        Difficulty difficulty,
        double speedPercent,
        int songLevel = 5);
};
