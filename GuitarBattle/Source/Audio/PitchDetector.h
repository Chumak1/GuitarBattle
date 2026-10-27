#pragma once

#include <JuceHeader.h>
#include <vector>
#include <algorithm>
#include <cmath>

class PitchDetector
{
public:

    double detectRms(
        const float* samples,
        int numSamples) const;

    // Optional frequency window. Gameplay narrows it around the current
    // target so notes above the 12th fret (which exceed the old 700 Hz cap)
    // are detected without risking octave errors on low strings.
    double detectPitch(
        const float* samples,
        int numSamples,
        double sampleRate,
        double minFrequency = 60.0,
        double maxFrequency = 700.0) const;

    int frequencyToMidi(
        double frequency) const;

    // Returns MIDI pitches found in a polyphonic signal, strongest first.
    // This is an additional analysis path; detectPitch() remains unchanged
    // for the existing single-note gameplay mode.
    inline std::vector<int> detectMultipleMidiNotes(
        const float* samples,
        int numSamples,
        double sampleRate,
        int maxNotes) const
    {
        std::vector<int> notes;
        if (samples == nullptr || numSamples < 1024 || sampleRate <= 0.0 || maxNotes <= 0)
            return notes;
    
        // Use the most recent power-of-two window (up to 8192 samples).
        int fftOrder = 10;
        while (fftOrder < 13 && (1 << (fftOrder + 1)) <= numSamples)
            ++fftOrder;
        const int fftSize = 1 << fftOrder;
        const int offset = numSamples - fftSize;
    
        juce::dsp::FFT fft(fftOrder);
        std::vector<float> fftData(static_cast<size_t>(fftSize * 2), 0.0f);
        double mean = 0.0;
        for (int i = offset; i < numSamples; ++i)
            mean += samples[i];
        mean /= static_cast<double>(fftSize);
    
        for (int i = 0; i < fftSize; ++i)
        {
            const double window = 0.5 - 0.5 * std::cos(
                juce::MathConstants<double>::twoPi * i / (fftSize - 1));
            fftData[static_cast<size_t>(i)] = static_cast<float>((samples[offset + i] - mean) * window);
        }
        fft.performFrequencyOnlyForwardTransform(fftData.data());
    
        struct Peak { double magnitude; int midi; double frequency; };
        std::vector<Peak> peaks;
        const int firstBin = std::max(1, static_cast<int>(std::ceil(65.0 * fftSize / sampleRate)));
        const int lastBin = std::min(fftSize / 2 - 1, static_cast<int>(std::floor(1400.0 * fftSize / sampleRate)));
        double maxMagnitude = 0.0;
        for (int bin = firstBin; bin <= lastBin; ++bin)
            maxMagnitude = std::max(maxMagnitude, static_cast<double>(fftData[static_cast<size_t>(bin)]));
        if (maxMagnitude <= 1.0e-6)
            return notes;
    
        for (int bin = firstBin + 1; bin < lastBin; ++bin)
        {
            const double magnitude = fftData[static_cast<size_t>(bin)];
            if (magnitude < maxMagnitude * 0.12
                || magnitude <= fftData[static_cast<size_t>(bin - 1)]
                || magnitude < fftData[static_cast<size_t>(bin + 1)])
                continue;
            const double frequency = static_cast<double>(bin) * sampleRate / fftSize;
            const int midi = frequencyToMidi(frequency);
            if (midi >= 30 && midi <= 90)
                peaks.push_back({ magnitude, midi, frequency });
        }
    
        std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b)
        {
            return a.magnitude > b.magnitude;
        });
    
        // Keep distinct semitones. Reject a candidate if a much stronger peak is
        // near one of its low-order harmonics (common guitar overtone false hits).
        for (const auto& candidate : peaks)
        {
            bool duplicate = false;
            for (const auto& existing : peaks)
            {
                if (&existing == &candidate) break;
                if (std::abs(existing.midi - candidate.midi) <= 1)
                {
                    duplicate = true;
                    break;
                }
                for (int harmonic = 2; harmonic <= 4; ++harmonic)
                {
                    const double ratio = candidate.frequency / existing.frequency;
                    if (std::abs(ratio - harmonic) < 0.035 && existing.magnitude > candidate.magnitude * 1.4)
                        duplicate = true;
                }
                if (duplicate) break;
            }
            if (!duplicate && std::find(notes.begin(), notes.end(), candidate.midi) == notes.end())
                notes.push_back(candidate.midi);
            if (static_cast<int>(notes.size()) >= maxNotes)
                break;
        }
        return notes;
    }

    juce::String frequencyToNote(
        double frequency) const;

private:
    // Scratch storage reused by detectPitch() so pitch analysis does not
    // allocate on every audio buffer.
    mutable std::vector<double> signalBuffer;
    mutable std::vector<double> correlationBuffer;
};