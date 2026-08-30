#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
/**
    Resonance Suppressor - Geruest.

    Aktuell reines Pass-Through. Die Parameter existieren bereits, damit die
    Host-Automation und der Preset-State von Anfang an stabil sind. Das DSP
    kommt im naechsten Schritt in eine eigene Klasse.
*/
class ResonanceSuppressorProcessor : public juce::AudioProcessor
{
public:
    ResonanceSuppressorProcessor();
    ~ResonanceSuppressorProcessor() override = default;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                             { return true; }

    const juce::String getName() const override                 { return JucePlugin_Name; }
    bool acceptsMidi() const override                           { return false; }
    bool producesMidi() const override                          { return false; }
    bool isMidiEffect() const override                          { return false; }
    double getTailLengthSeconds() const override                { return 0.0; }

    int getNumPrograms() override                               { return 1; }
    int getCurrentProgram() override                            { return 0; }
    void setCurrentProgram (int) override                       {}
    const juce::String getProgramName (int) override            { return {}; }
    void changeProgramName (int, const juce::String&) override  {}

    //==========================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Zeiger auf haeufig gelesene Parameter. atomar, damit der Audio-Thread
    // sie ohne Sperren lesen kann.
    std::atomic<float>* depthParam   { nullptr };
    std::atomic<float>* detailParam  { nullptr };
    std::atomic<float>* attackParam  { nullptr };
    std::atomic<float>* releaseParam { nullptr };
    std::atomic<float>* mixParam     { nullptr };
    std::atomic<float>* outputParam  { nullptr };

    juce::SmoothedValue<float> outputGain;

    double currentSampleRate { 44100.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ResonanceSuppressorProcessor)
};
