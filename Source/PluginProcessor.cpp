#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
namespace ParamID
{
    static constexpr auto depth   = "depth";
    static constexpr auto detail  = "detail";
    static constexpr auto attack  = "attack";
    static constexpr auto release = "release";
    static constexpr auto maxcut  = "maxcut";
    static constexpr auto mix     = "mix";
    static constexpr auto output  = "output";
    static constexpr auto bypass  = "bypass";
    static constexpr auto delta   = "delta";
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
    maxCutParam  = apvts.getRawParameterValue (ParamID::maxcut);
    mixParam     = apvts.getRawParameterValue (ParamID::mix);
    outputParam  = apvts.getRawParameterValue (ParamID::output);
    bypassParam  = apvts.getRawParameterValue (ParamID::bypass);
    deltaParam   = apvts.getRawParameterValue (ParamID::delta);
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
        ParameterID { ParamID::maxcut, 1 }, "Max Cut",
        NormalisableRange<float> (0.0f, 40.0f, 0.1f), 40.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::mix, 1 }, "Mix",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::output, 1 }, "Output",
        NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ParamID::bypass, 1 }, "Bypass", false));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ParamID::delta, 1 }, "Delta", false));

    return layout;
}

//==============================================================================
void ResonanceSuppressorProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    const int numCh = juce::jlimit (1, maxChannels, getTotalNumInputChannels());
    activeChannels.store (numCh);

    for (auto& e : engines)
        e.prepare (sampleRate);

    delayLength = SpectralEngine::getLatencySamples();
    delayLine.setSize (maxChannels, delayLength);
    delayLine.clear();
    delayWritePos = 0;

    dryBuffer.setSize (maxChannels, juce::jmax (1, samplesPerBlock));
    dryBuffer.clear();

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    mixAmount.reset (sampleRate, 0.02);
    mixAmount.setCurrentAndTargetValue (mixParam->load() * 0.01f);

    setLatencySamples (SpectralEngine::getLatencySamples());
}

void ResonanceSuppressorProcessor::releaseResources()
{
}

bool ResonanceSuppressorProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (in.isDisabled() || out.isDisabled() || in != out)
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

    const int numIn      = getTotalNumInputChannels();
    const int numOut     = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, numSamples);

    const int numCh = juce::jmin (maxChannels, numIn);

    if (numCh <= 0 || delayLength <= 0 || numSamples <= 0)
        return;

    if (dryBuffer.getNumSamples() < numSamples)
        dryBuffer.setSize (maxChannels, numSamples, false, false, true);

    for (int ch = 0; ch < numCh; ++ch)
        dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    // ---- Trockensignal um die Engine-Latenz verzoegern -------------------
    {
        int wp = delayWritePos;

        for (int n = 0; n < numSamples; ++n)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float in  = dryBuffer.getSample (ch, n);
                const float out = delayLine.getSample (ch, wp);
                delayLine.setSample (ch, wp, in);
                dryBuffer.setSample (ch, n, out);
            }

            if (++wp >= delayLength)
                wp = 0;
        }

        delayWritePos = wp;
    }

    // ---- Wet-Pfad -------------------------------------------------------
    const float depth   = depthParam->load();
    const float detail  = detailParam->load();
    const float attack  = attackParam->load();
    const float release = releaseParam->load();
    const float maxCut  = maxCutParam->load();

    for (int ch = 0; ch < numCh; ++ch)
    {
        engines[(size_t) ch].setParameters (depth, detail, attack, release, maxCut);
        engines[(size_t) ch].process (buffer.getWritePointer (ch), numSamples);
    }

    // ---- Delta / Mix / Bypass / Output ----------------------------------
    const bool bypassed = bypassParam->load() > 0.5f;
    const bool delta    = deltaParam->load() > 0.5f;

    mixAmount .setTargetValue (mixParam->load() * 0.01f);
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    for (int n = 0; n < numSamples; ++n)
    {
        const float mix = mixAmount.getNextValue();
        const float g   = outputGain.getNextValue();

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float dry = dryBuffer.getSample (ch, n);
            const float wet = buffer.getSample (ch, n);

            float y;

            if (bypassed)   y = dry;
            else if (delta) y = (dry - wet) * g;
            else            y = (dry + mix * (wet - dry)) * g;

            buffer.setSample (ch, n, y);
        }
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
    return new ResonanceSuppressorEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ResonanceSuppressorProcessor();
}
