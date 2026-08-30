#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <cmath>

//==============================================================================
/**
    Hochaufloesender spektraler Resonanz-Suppressor fuer einen Kanal.

    Ablauf pro Frame:
      1. FFT des gefensterten Eingangs
      2. Magnitude in dB
      3. lokaler spektraler Durchschnitt (gleitendes Fenster mit konstanter
         relativer Bandbreite, ueber Praefixsummen in O(1) pro Bin)
      4. prominence = magDb - localAvgDb
      5. alles oberhalb einer Schwelle wird zur Absenkung
      6. zeitliche Glaettung pro Bin, Attack/Release frequenzabhaengig
      7. Gain-Maske leicht ueber die Frequenz glaetten (gegen Ringing)
      8. IFFT, Fenster, Overlap-Add

    Die Anzahl unabhaengiger Absenkungen entspricht der Anzahl der Bins,
    nicht einer festen Bandzahl.
*/
class SpectralEngine
{
public:
    //==========================================================================
    // Zentrale Konstanten. Groesseres fftOrder = feinere Frequenzaufloesung,
    // aber mehr Latenz und traegere Zeitaufloesung.
    static constexpr int fftOrder = 11;              // 2^11 = 2048
    static constexpr int fftSize  = 1 << fftOrder;
    static constexpr int hopSize  = fftSize / 4;     // 75 % Overlap
    static constexpr int numBins  = fftSize / 2 + 1;

    // Summe der quadrierten Hann-Fenster bei 75 % Overlap
    static constexpr float olaNorm = 1.0f / 1.5f;

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

        magDb        .assign ((size_t) numBins, -140.0f);
        prefix       .assign ((size_t) numBins + 1, 0.0f);
        reduction    .assign ((size_t) numBins, 0.0f);
        gains        .assign ((size_t) numBins, 1.0f);
        gainsTmp     .assign ((size_t) numBins, 1.0f);
        attackCoef   .assign ((size_t) numBins, 0.5f);
        releaseCoef  .assign ((size_t) numBins, 0.2f);
        binFreq      .assign ((size_t) numBins, 0.0f);

        for (int k = 0; k < numBins; ++k)
            binFreq[(size_t) k] = (float) (k * sr / (double) fftSize);

        reset();
        updateTimeCoefficients (true);
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

    //==========================================================================
    /** Alle Werte 0..100 ausser maxCutDb. Aufruf pro Block, nicht pro Sample. */
    void setParameters (float depth, float detail, float attack, float release, float maxCutDb)
    {
        depth01  = juce::jlimit (0.0f, 1.0f, depth  * 0.01f);
        detail01 = juce::jlimit (0.0f, 1.0f, detail * 0.01f);
        maxCut   = juce::jlimit (0.0f, 40.0f, maxCutDb);

        if (attack != lastAttack || release != lastRelease)
        {
            lastAttack  = attack;
            lastRelease = release;
            updateTimeCoefficients (false);
        }
    }

    //==========================================================================
    /** In-place. Ergebnis ist das reine Wet-Signal, um fftSize verzoegert. */
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
    void updateTimeCoefficients (bool force)
    {
        juce::ignoreUnused (force);

        const float hopTime = (float) hopSize / (float) sr;

        // 0.5 ms .. 50 ms  bzw.  10 ms .. 500 ms
        const float attackSec  = 0.0005f * std::pow (100.0f, lastAttack  * 0.01f);
        const float releaseSec = 0.010f  * std::pow (50.0f,  lastRelease * 0.01f);

        for (int k = 0; k < numBins; ++k)
        {
            // Hohe Frequenzen duerfen schneller bearbeitet werden als tiefe.
            const float f     = juce::jmax (30.0f, binFreq[(size_t) k]);
            const float scale = juce::jlimit (0.35f, 4.0f, 1000.0f / f);

            const float ta = attackSec  * scale;
            const float tr = releaseSec * scale;

            attackCoef [(size_t) k] = 1.0f - std::exp (-hopTime / juce::jmax (1.0e-5f, ta));
            releaseCoef[(size_t) k] = 1.0f - std::exp (-hopTime / juce::jmax (1.0e-5f, tr));
        }
    }

    //==========================================================================
    void processFrame()
    {
        // ---- Fensterung -------------------------------------------------
        for (int i = 0; i < fftSize; ++i)
        {
            int idx = pos + i;
            if (idx >= fftSize) idx -= fftSize;
            fftData[(size_t) i] = inputBuffer[(size_t) idx] * window[(size_t) i];
        }

        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
        fft.performRealOnlyForwardTransform (fftData.data());

        auto* cplx = reinterpret_cast<juce::dsp::Complex<float>*> (fftData.data());

        // ---- Magnituden in dB -------------------------------------------
        float frameMax = -200.0f;

        for (int k = 0; k < numBins; ++k)
        {
            const float m = std::abs (cplx[k]);
            const float d = 20.0f * std::log10 (m + 1.0e-9f);
            magDb[(size_t) k] = d;
            frameMax = juce::jmax (frameMax, d);
        }

        // Alles mehr als 80 dB unter dem Frame-Peak ist Rauschen und wird
        // nicht bearbeitet. Verhindert Chasing in leisen Passagen.
        const float noiseFloor = frameMax - 80.0f;

        // ---- Praefixsummen fuer den lokalen Durchschnitt ------------------
        prefix[0] = 0.0f;
        for (int k = 0; k < numBins; ++k)
            prefix[(size_t) k + 1] = prefix[(size_t) k] + magDb[(size_t) k];

        // Detail hoch = enges Vergleichsfenster = nur schmale Peaks fallen auf.
        const float relWidth = juce::jmap (detail01, 0.60f, 0.04f);
        const int   minWidth = juce::jmax (2, juce::roundToInt (juce::jmap (detail01, 24.0f, 3.0f)));

        // Depth steuert Schwelle und Steigung gemeinsam.
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
                    const float localAvg  = (prefix[(size_t) hi] - prefix[(size_t) lo]) / (float) n;
                    const float prominence = magDb[(size_t) k] - localAvg;
                    const float excess     = prominence - thresholdDb;

                    if (excess > 0.0f)
                        target = juce::jmin (excess * strength, maxCut);
                }
            }

            // ---- zeitliche Glaettung ------------------------------------
            const float prev = reduction[(size_t) k];
            const float c    = (target > prev) ? attackCoef[(size_t) k]
                                               : releaseCoef[(size_t) k];
            reduction[(size_t) k] = prev + c * (target - prev);

            gainsTmp[(size_t) k] = std::pow (10.0f, -reduction[(size_t) k] / 20.0f);
        }

        // ---- Gain-Maske ueber die Frequenz glaetten -----------------------
        gains[0] = gainsTmp[0];
        gains[(size_t) numBins - 1] = gainsTmp[(size_t) numBins - 1];

        for (int k = 1; k < numBins - 1; ++k)
            gains[(size_t) k] = 0.25f * gainsTmp[(size_t) k - 1]
                              + 0.50f * gainsTmp[(size_t) k]
                              + 0.25f * gainsTmp[(size_t) k + 1];

        // ---- anwenden, inklusive konjugiert gespiegelter Haelfte ----------
        for (int k = 0; k < numBins; ++k)
        {
            cplx[k] *= gains[(size_t) k];

            if (k > 0 && k < fftSize / 2)
                cplx[fftSize - k] = std::conj (cplx[k]);
        }

        fft.performRealOnlyInverseTransform (fftData.data());

        // ---- Overlap-Add -------------------------------------------------
        for (int i = 0; i < fftSize; ++i)
        {
            int idx = pos + i;
            if (idx >= fftSize) idx -= fftSize;
            outputBuffer[(size_t) idx] += fftData[(size_t) i] * window[(size_t) i] * olaNorm;
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

    JUCE_LEAK_DETECTOR (SpectralEngine)
};
