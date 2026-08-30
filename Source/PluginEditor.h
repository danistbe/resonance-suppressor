#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

class ResonanceSuppressorProcessor;

//==============================================================================
/** Echtzeit-Anzeige: Eingangsspektrum im Hintergrund, Gain Reduction darueber. */
class ReductionGraph : public juce::Component,
                       private juce::Timer
{
public:
    explicit ReductionGraph (ResonanceSuppressorProcessor& p);
    ~ReductionGraph() override = default;

    void paint (juce::Graphics& g) override;
    void resized() override;

    void setMaxCut (float db) { maxCutDb = db; }

private:
    void timerCallback() override;
    void rebuildBinLookup();

    static constexpr float minFreq = 20.0f;
    static constexpr float maxFreq = 20000.0f;
    static constexpr float maxDisplayDb = 40.0f;

    float freqToX (float hz) const;

    ResonanceSuppressorProcessor& proc;

    std::vector<int>   binForPixel;    // pro Pixel der zugehoerige Startbin
    std::vector<float> reductionPx;    // geglaettete Anzeigewerte pro Pixel
    std::vector<float> spectrumPx;

    float maxCutDb { 40.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReductionGraph)
};

//==============================================================================
class ResonanceSuppressorEditor : public juce::AudioProcessorEditor,
                                  private juce::Timer
{
public:
    explicit ResonanceSuppressorEditor (ResonanceSuppressorProcessor&);
    ~ResonanceSuppressorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<SliderAttachment> attachment;
    };

    void setupKnob (Knob& k, const juce::String& paramID, const juce::String& text);

    ResonanceSuppressorProcessor& proc;

    ReductionGraph graph;

    Knob depth, detail, attack, release, maxCut, mix, output;

    juce::TextButton bypassButton { "BYPASS" };
    juce::TextButton deltaButton  { "DELTA" };
    std::unique_ptr<ButtonAttachment> bypassAttachment, deltaAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResonanceSuppressorEditor)
};
