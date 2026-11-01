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

    // Keeps the existing gameplay rules: exact MIDI pitch match and a strict
    // +/-150 ms timing window. Pitch deviation still determines score quality.
    static NoteEvaluation evaluateNote(
        int targetMidi,
        int detectedMidi,
        double detectedFrequency,
        double timingErrorMs,
        Difficulty difficulty,
        double speedPercent,
        int songLevel = 5);
};
