#pragma once

#include <atomic>
#include <cmath>
#include "ScoringSystem.h"

// Runtime counters for one gameplay session. Thread-safe because note detection
// can update statistics from the audio callback while the UI reads the score.
class GameplayStats
{
public:
    void reset()
    {
        score.store(0, std::memory_order_relaxed);
        hitCount.store(0, std::memory_order_relaxed);
        missCount.store(0, std::memory_order_relaxed);
        perfectCount.store(0, std::memory_order_relaxed);
        greatCount.store(0, std::memory_order_relaxed);
        goodCount.store(0, std::memory_order_relaxed);
        combo.store(0, std::memory_order_relaxed);
        maxCombo.store(0, std::memory_order_relaxed);
        comboBonusPoints.store(0, std::memory_order_relaxed);
        timingAbsMicros.store(0, std::memory_order_relaxed);
        timingSamples.store(0, std::memory_order_relaxed);
    }

    // Absolute timing error of a successfully played note (milliseconds).
    // The server uses the average as the player's timing metric.
    void recordTiming(double timingErrorMs)
    {
        if (! std::isfinite(timingErrorMs))
            return;

        const auto micros = static_cast<juce::int64>(
            std::llround(std::abs(timingErrorMs) * 1000.0));

        timingAbsMicros.fetch_add(micros, std::memory_order_relaxed);
        timingSamples.fetch_add(1, std::memory_order_relaxed);
    }

    int recordHit(int points, HitQuality quality)
    {
        switch (quality)
        {
        case HitQuality::Perfect: perfectCount.fetch_add(1, std::memory_order_relaxed); break;
        case HitQuality::Great:  greatCount.fetch_add(1, std::memory_order_relaxed); break;
        case HitQuality::Good:   goodCount.fetch_add(1, std::memory_order_relaxed); break;
        case HitQuality::Miss:   recordMiss(); return getScore();
        }

        hitCount.fetch_add(1, std::memory_order_relaxed);

        const int currentCombo = combo.fetch_add(1, std::memory_order_relaxed) + 1;
        int previousMax = maxCombo.load(std::memory_order_relaxed);
        while (currentCombo > previousMax
            && !maxCombo.compare_exchange_weak(previousMax, currentCombo,
                std::memory_order_relaxed, std::memory_order_relaxed))
        {
        }

        // Small, transparent bonus for sustained accuracy: +5 points for
        // every combo step after the first (x2 = +5, x3 = +10, etc.).
        const int bonus = (currentCombo - 1) * 5;
        comboBonusPoints.fetch_add(bonus, std::memory_order_relaxed);
        const int awardedPoints = points + bonus;
        return score.fetch_add(awardedPoints, std::memory_order_relaxed) + awardedPoints;
    }

    void recordMiss()
    {
        missCount.fetch_add(1, std::memory_order_relaxed);
        combo.store(0, std::memory_order_relaxed);
    }
    int getScore() const { return score.load(std::memory_order_relaxed); }
    int getHitCount() const { return hitCount.load(std::memory_order_relaxed); }
    int getMissCount() const { return missCount.load(std::memory_order_relaxed); }
    int getPerfectCount() const { return perfectCount.load(std::memory_order_relaxed); }
    int getGreatCount() const { return greatCount.load(std::memory_order_relaxed); }
    int getGoodCount() const { return goodCount.load(std::memory_order_relaxed); }
    int getCombo() const { return combo.load(std::memory_order_relaxed); }
    int getMaxCombo() const { return maxCombo.load(std::memory_order_relaxed); }
    int getComboBonusPoints() const { return comboBonusPoints.load(std::memory_order_relaxed); }

    // Average absolute timing error in milliseconds, or -1 when no timing
    // samples were recorded.
    double getAverageTimingErrorMs() const
    {
        const auto samples = timingSamples.load(std::memory_order_relaxed);

        if (samples <= 0)
            return -1.0;

        const auto micros = timingAbsMicros.load(std::memory_order_relaxed);
        return (static_cast<double>(micros) / 1000.0)
            / static_cast<double>(samples);
    }

private:
    std::atomic<int> score{ 0 };
    std::atomic<int> hitCount{ 0 };
    std::atomic<int> missCount{ 0 };
    std::atomic<int> perfectCount{ 0 };
    std::atomic<int> greatCount{ 0 };
    std::atomic<int> goodCount{ 0 };
    std::atomic<int> combo{ 0 };
    std::atomic<int> maxCombo{ 0 };
    std::atomic<int> comboBonusPoints{ 0 };
    std::atomic<juce::int64> timingAbsMicros{ 0 };
    std::atomic<int> timingSamples{ 0 };
};
