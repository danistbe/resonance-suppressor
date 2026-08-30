#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <array>
#include <atomic>
#include <cmath>

//==============================================================================
/**
    Hochaufloesender spektraler Resonanz-Suppressor fuer einen Kanal.

    Die Anzahl unabhaengiger Absenkungen entspricht der Anzahl der Bins,
    nicht einer festen Bandzahl.

    Fuer die Darstellung schreibt die Engine zwei Snapshots in Atomic-Arrays.
    Der Audio-Thread schreibt nur, der Message-Thread liest nur. Beides
    relaxed - kurzzeitig inkonsistente Werte sind fuer eine Anzeige egal.
*/
class SpectralEngine
{
public:
    //==========================================================================
    static constexpr int fftOrder = 11;              // 2^11 = 2048
    static constexpr int fftSize  = 1 << fftOrder;
    static constexpr int hopSize  = fftSize / 4;     // 75 % Overlap
    static constexpr int numBins  = fftSize / 2 + 1;

    static constexpr float olaNorm = 1.0f / 1.5f;

    //==========================================================================
    SpectralEngine()
    {
        for (auto& v : displayReduction) v.store (0.0f,   std::memory_order_relaxed);
        for (auto& v : displaySpectrum)  v.store (-100.0f, std::memory_order_relaxed);
    }

    //==========================================================================
    void prepare (double sampleRate)
    {
        sr = sampleRate;

        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                         * (float) i / (float) fftSize);

        inputBuffer .assign ((size_t) fftSize, 0.0f);
        outputBuffer.assign ((size_t) fftSize, 0.0f);
        fftData     .assign ((size_t) fftSize * 2, 0.0f);

        magDb       .assign ((size_t) numBins, -140.0f);
        prefix      .assign ((size_t) numBins + 1, 0.0f);
        reduction   .assign ((size_t) numBins, 0.0f);
        gains       .assign ((size_t) numBins, 1.0f);
        gainsTmp    .assign ((size_t) numBins, 1.0f);
        attackCoef  .assign ((size_t) numBins, 0.5f);
        releaseCoef .assign ((size_t) numBins, 0.2f);
        binFreq     .assign ((size_t) numBins, 0.0f);

        for (int k = 0; k < numBins; ++k)
            binFreq[(size_t) k] = (float) (k * sr / (double) fftSize);

        reset();
        updateTimeCoefficients();
    }

    void reset()
    {
        std::fill (inputBuffer .begin(), inputBuffer .end(), 0.0f);
        std::fill (outputBuffer.begin(), outputBuffer.end(), 0.0f);
        std::fill (reduction   .begin(), reduction   .end(), 0.0f);
        std::fill (gains       .begin(), gains       .end(), 1.0f);
        pos = 0;
        hopCounter = 0;
    }

    static int getLatencySamples()  { return fftSize; }

    double getSampleRate() const noexcept { return sr; }

    /** Absenkung in dB, positiv. Nur fuer die Anzeige. */
    float getDisplayReduction (int k) const noexcept
    {
        return displayReduction[(size_t) k].load (std::memory_order_relaxed);
    }

    /** Magnitude relativ zum Frame-Peak, also -100..0 dB. Nur fuer die Anzeige. */
    float getDisplaySpectrum (int k) const noexcept
    {
        return displaySpectrum[(size_t) k].load (std::memory_order_relaxed);
    }

    //==========================================================================
    void setParameters (float depth, float detail, float attack, float release, float maxCutDb)
    {
        depth01  = juce::jlimit (0.0f, 1.0f, depth  * 0.01f);
        detail01 = juce::jlimit (0.0f, 1.0f, detail * 0.01f);
        maxCut   = juce::jlimit (0.0f, 40.0f, maxCutDb);

        if (attack != lastAttack || release != lastRelease)
        {
            lastAttack  = attack;
            lastRelease = release;
            updateTimeCoefficients();
        }
    }

    //==========================================================================
    void process (float* data, int numSamples)
    {
        for (int n = 0; n < numSamples; ++n)
        {
            inputBuffer[(size_t) pos] = data[n];
            data[n] = outputBuffer[(size_t) pos];
            outputBuffer[(size_t) pos] = 0.0f;

            if (++pos >= fftSize)
                pos = 0;

            if (++hopCounter >= hopSize)
            {
                hopCounter = 0;
                processFrame();
            }
        }
    }

private:
    //==========================================================================
    void updateTimeCoefficients()
    {
        const float hopTime = (float) hopSize / (float) sr;

        const float attackSec  = 0.0005f * std::pow (100.0f, lastAttack  * 0.01f);
        const float releaseSec = 0.010f  * std::pow (50.0f,  lastRelease * 0.01f);

        for (int k = 0; k < numBins; ++k)
        {
            const float f     = juce::jmax (30.0f, binFreq[(size_t) k]);
            const float scale = juce::jlimit (0.35f, 4.0f, 1000.0f / f);

            attackCoef [(size_t) k] = 1.0f - std::exp (-hopTime / juce::jmax (1.0e-5f, attackSec  * scale));
            releaseCoef[(size_t) k] = 1.0f - std::exp (-hopTime / juce::jmax (1.0e-5f, releaseSec * scale));
        }
    }

    //==========================================================================
    void processFrame()
    {
        for (int i = 0; i < fftSize; ++i)
        {
            int idx = pos + i;
            if (idx >= fftSize) idx -= fftSize;
            fftData[(size_t) i] = inputBuffer[(size_t) idx] * window[(size_t) i];
        }

        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
        fft.performRealOnlyForwardTransform (fftData.data());

        auto* cplx = reinterpret_cast<juce::dsp::Complex<float>*> (fftData.data());

        float frameMax = -200.0f;

        for (int k = 0; k < numBins; ++k)
        {
            const float m = std::abs (cplx[k]);
            const float d = 20.0f * std::log10 (m + 1.0e-9f);
            magDb[(size_t) k] = d;
            frameMax = juce::jmax (frameMax, d);
        }

        const float noiseFloor = frameMax - 80.0f;

        prefix[0] = 0.0f;
        for (int k = 0; k < numBins; ++k)
            prefix[(size_t) k + 1] = prefix[(size_t) k] + magDb[(size_t) k];

        const float relWidth = juce::jmap (detail01, 0.60f, 0.04f);
        const int   minWidth = juce::jmax (2, juce::roundToInt (juce::jmap (detail01, 24.0f, 3.0f)));

        const float thresholdDb = juce::jmap (depth01, 12.0f, 1.5f);
        const float strength    = juce::jmap (depth01, 0.25f, 1.40f);

        for (int k = 0; k < numBins; ++k)
        {
            float target = 0.0f;

            if (magDb[(size_t) k] > noiseFloor && k > 1)
            {
                const int half = juce::jmax (minWidth, juce::roundToInt ((float) k * relWidth));
                const int lo   = juce::jmax (1, k - half);
                const int hi   = juce::jmin (numBins - 1, k + half);
                const int n    = hi - lo;

                if (n > 0)
                {
                    const float localAvg   = (prefix[(size_t) hi] - prefix[(size_t) lo]) / (float) n;
                    const float prominence = magDb[(size_t) k] - localAvg;
                    const float excess     = prominence - thresholdDb;

                    if (excess > 0.0f)
                        target = juce::jmin (excess * strength, maxCut);
                }
            }

            const float prev = reduction[(size_t) k];
            const float c    = (target > prev) ? attackCoef[(size_t) k]
                                               : releaseCoef[(size_t) k];
            reduction[(size_t) k] = prev + c * (target - prev);

            gainsTmp[(size_t) k] = std::pow (10.0f, -reduction[(size_t) k] / 20.0f);
        }

        gains[0] = gainsTmp[0];
        gains[(size_t) numBins - 1] = gainsTmp[(size_t) numBins - 1];

        for (int k = 1; k < numBins - 1; ++k)
            gains[(size_t) k] = 0.25f * gainsTmp[(size_t) k - 1]
                              + 0.50f * gainsTmp[(size_t) k]
                              + 0.25f * gainsTmp[(size_t) k + 1];

        for (int k = 0; k < numBins; ++k)
        {
            cplx[k] *= gains[(size_t) k];

            if (k > 0 && k < fftSize / 2)
                cplx[fftSize - k] = std::conj (cplx[k]);
        }

        fft.performRealOnlyInverseTransform (fftData.data());

        for (int i = 0; i < fftSize; ++i)
        {
            int idx = pos + i;
            if (idx >= fftSize) idx -= fftSize;
            outputBuffer[(size_t) idx] += fftData[(size_t) i] * window[(size_t) i] * olaNorm;
        }

        // ---- Snapshots fuer die Anzeige ----------------------------------
        for (int k = 0; k < numBins; ++k)
        {
            displayReduction[(size_t) k].store (reduction[(size_t) k], std::memory_order_relaxed);
            displaySpectrum [(size_t) k].store (juce::jmax (-100.0f, magDb[(size_t) k] - frameMax),
                                                std::memory_order_relaxed);
        }
    }

    //==========================================================================
    juce::dsp::FFT fft { fftOrder };

    double sr { 44100.0 };
    int pos { 0 };
    int hopCounter { 0 };

    float depth01 { 0.2f };
    float detail01 { 0.5f };
    float maxCut { 40.0f };
    float lastAttack { 50.0f };
    float lastRelease { 50.0f };

    std::vector<float> window, inputBuffer, outputBuffer, fftData;
    std::vector<float> magDb, prefix, reduction, gains, gainsTmp;
    std::vector<float> attackCoef, releaseCoef, binFreq;

    std::array<std::atomic<float>, (size_t) numBins> displayReduction;
    std::array<std::atomic<float>, (size_t) numBins> displaySpectrum;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralEngine)
};
