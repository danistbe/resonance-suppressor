#include "PluginProcessor.h"

//==============================================================================
namespace ParamID
{
    static constexpr auto depth   = "depth";
    static constexpr auto detail  = "detail";
    static constexpr auto attack  = "attack";
    static constexpr auto release = "release";
    static constexpr auto mix     = "mix";
    static constexpr auto output  = "output";
    static constexpr auto bypass  = "bypass";
}

//==============================================================================
ResonanceSuppressorProcessor::ResonanceSuppressorProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    depthParam   = apvts.getRawParameterValue (ParamID::depth);
    detailParam  = apvts.getRawParameterValue (ParamID::detail);
    attackParam  = apvts.getRawParameterValue (ParamID::attack);
    releaseParam = apvts.getRawParameterValue (ParamID::release);
    mixParam     = apvts.getRawParameterValue (ParamID::mix);
    outputParam  = apvts.getRawParameterValue (ParamID::output);
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
ResonanceSuppressorProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::depth, 1 }, "Depth",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 20.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::detail, 1 }, "Detail",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::attack, 1 }, "Attack",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::release, 1 }, "Release",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::mix, 1 }, "Mix",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::output, 1 }, "Output",
        NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ParamID::bypass, 1 }, "Bypass", false));

    return layout;
}

//==============================================================================
void ResonanceSuppressorProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);

    currentSampleRate = sampleRate;

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    // Noch keine zusaetzliche Latenz - kommt mit der STFT-Engine.
    setLatencySamples (0);
}

void ResonanceSuppressorProcessor::releaseResources()
{
}

bool ResonanceSuppressorProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (in.isDisabled() || out.isDisabled())
        return false;

    if (in != out)
        return false;

    return in == juce::AudioChannelSet::mono()
        || in == juce::AudioChannelSet::stereo();
}

//==============================================================================
void ResonanceSuppressorProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                 juce::MidiBuffer& midi)
{
    juce::ignoreUnused (midi);

    juce::ScopedNoDenormals noDenormals;

    const auto numIn  = getTotalNumInputChannels();
    const auto numOut = getTotalNumOutputChannels();

    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // ------------------------------------------------------------------
    // Hier kommt spaeter das eigentliche Processing hin.
    // Aktuell nur Output Gain, damit die Signalkette schon steht.
    // ------------------------------------------------------------------

    outputGain.setTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const auto g = outputGain.getNextValue();

        for (int ch = 0; ch < numOut; ++ch)
            buffer.getWritePointer (ch)[i] *= g;
    }
}

//==============================================================================
void ResonanceSuppressorProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void ResonanceSuppressorProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessorEditor* ResonanceSuppressorProcessor::createEditor()
{
    // Vorlaeufige Oberflaeche. Wird spaeter durch den Frequenzgraph ersetzt.
    return new juce::GenericAudioProcessorEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ResonanceSuppressorProcessor();
}
