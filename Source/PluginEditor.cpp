#include "PluginEditor.h"
#include "PluginProcessor.h"

//==============================================================================
namespace Colours
{
    static const juce::Colour background   { 0xff141719 };
    static const juce::Colour panel        { 0xff1c2124 };
    static const juce::Colour grid         { 0xff2c3438 };
    static const juce::Colour gridText     { 0xff5f6b70 };
    static const juce::Colour spectrum     { 0xff2f4a52 };
    static const juce::Colour reduction    { 0xff4fd1c5 };
    static const juce::Colour maxCutLine   { 0xffd97757 };
    static const juce::Colour text         { 0xffc9d1d3 };
}

//==============================================================================
ReductionGraph::ReductionGraph (ResonanceSuppressorProcessor& p)
    : proc (p)
{
    startTimerHz (30);
}

float ReductionGraph::freqToX (float hz) const
{
    const float a = std::log (hz / minFreq);
    const float b = std::log (maxFreq / minFreq);
    return (a / b) * (float) getWidth();
}

void ReductionGraph::resized()
{
    rebuildBinLookup();
}

void ReductionGraph::rebuildBinLookup()
{
    const int w = juce::jmax (1, getWidth());

    binForPixel.assign ((size_t) w + 1, 0);
    reductionPx.assign ((size_t) w, 0.0f);
    spectrumPx .assign ((size_t) w, -100.0f);

    const double sr  = juce::jmax (8000.0, proc.getEngine (0).getSampleRate());
    const double hzPerBin = sr / (double) SpectralEngine::fftSize;

    for (int x = 0; x <= w; ++x)
    {
        const float t  = (float) x / (float) w;
        const float hz = minFreq * std::pow (maxFreq / minFreq, t);
        const int   k  = juce::jlimit (0, SpectralEngine::numBins - 1,
                                       (int) std::round (hz / hzPerBin));
        binForPixel[(size_t) x] = k;
    }
}

void ReductionGraph::timerCallback()
{
    const int w = getWidth();

    if (w <= 0 || (int) reductionPx.size() != w)
    {
        rebuildBinLookup();
        if ((int) reductionPx.size() != w)
            return;
    }

    const int numCh = juce::jmax (1, proc.getActiveChannels());

    for (int x = 0; x < w; ++x)
    {
        const int k0 = binForPixel[(size_t) x];
        const int k1 = juce::jmax (k0 + 1, binForPixel[(size_t) x + 1]);

        float red  = 0.0f;
        float spec = -100.0f;

        for (int ch = 0; ch < numCh; ++ch)
        {
            const auto& e = proc.getEngine (ch);

            for (int k = k0; k < k1 && k < SpectralEngine::numBins; ++k)
            {
                red  = juce::jmax (red,  e.getDisplayReduction (k));
                spec = juce::jmax (spec, e.getDisplaySpectrum (k));
            }
        }

        // Leichte Glaettung, damit die Anzeige nicht flackert.
        reductionPx[(size_t) x] += 0.5f * (red  - reductionPx[(size_t) x]);
        spectrumPx [(size_t) x] += 0.3f * (spec - spectrumPx [(size_t) x]);
    }

    repaint();
}

void ReductionGraph::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const float w = bounds.getWidth();
    const float h = bounds.getHeight();

    g.setColour (Colours::panel);
    g.fillRoundedRectangle (bounds, 4.0f);

    // ---- Frequenzraster --------------------------------------------------
    static const float gridFreqs[] = { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f,
                                       1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f };

    g.setFont (11.0f);

    for (auto f : gridFreqs)
    {
        const float x = freqToX (f);

        g.setColour (Colours::grid);
        g.drawVerticalLine ((int) x, 0.0f, h);

        g.setColour (Colours::gridText);
        const juce::String label = f >= 1000.0f ? juce::String (f / 1000.0f, 0) + "k"
                                                : juce::String ((int) f);
        g.drawText (label, (int) x + 3, (int) h - 15, 40, 14,
                    juce::Justification::left, false);
    }

    // ---- dB-Raster -------------------------------------------------------
    for (int db = 10; db < (int) maxDisplayDb; db += 10)
    {
        const float y = (float) db / maxDisplayDb * h;

        g.setColour (Colours::grid);
        g.drawHorizontalLine ((int) y, 0.0f, w);

        g.setColour (Colours::gridText);
        g.drawText ("-" + juce::String (db), 4, (int) y + 2, 30, 12,
                    juce::Justification::left, false);
    }

    if ((int) reductionPx.size() != (int) w || reductionPx.empty())
        return;

    // ---- Eingangsspektrum ------------------------------------------------
    {
        juce::Path p;
        p.startNewSubPath (0.0f, h);

        for (int x = 0; x < (int) w; ++x)
        {
            const float v = juce::jlimit (-100.0f, 0.0f, spectrumPx[(size_t) x]);
            const float y = h - ((v + 100.0f) / 100.0f) * h * 0.9f;
            p.lineTo ((float) x, y);
        }

        p.lineTo (w, h);
        p.closeSubPath();

        g.setColour (Colours::spectrum.withAlpha (0.55f));
        g.fillPath (p);
    }

    // ---- Max-Cut-Grenze --------------------------------------------------
    {
        const float y = juce::jlimit (0.0f, h, maxCutDb / maxDisplayDb * h);

        g.setColour (Colours::maxCutLine.withAlpha (0.7f));
        const float dashes[] = { 4.0f, 4.0f };
        g.drawDashedLine ({ 0.0f, y, w, y }, dashes, 2, 1.0f);
    }

    // ---- Gain Reduction --------------------------------------------------
    {
        juce::Path p;
        p.startNewSubPath (0.0f, 0.0f);

        for (int x = 0; x < (int) w; ++x)
        {
            const float v = juce::jlimit (0.0f, maxDisplayDb, reductionPx[(size_t) x]);
            p.lineTo ((float) x, v / maxDisplayDb * h);
        }

        p.lineTo (w, 0.0f);
        p.closeSubPath();

        g.setColour (Colours::reduction.withAlpha (0.28f));
        g.fillPath (p);

        juce::Path line;
        bool started = false;

        for (int x = 0; x < (int) w; ++x)
        {
            const float v = juce::jlimit (0.0f, maxDisplayDb, reductionPx[(size_t) x]);
            const float y = v / maxDisplayDb * h;

            if (! started) { line.startNewSubPath ((float) x, y); started = true; }
            else            line.lineTo ((float) x, y);
        }

        g.setColour (Colours::reduction);
        g.strokePath (line, juce::PathStrokeType (1.5f));
    }

    g.setColour (Colours::grid);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, 1.0f);
}

//==============================================================================
ResonanceSuppressorEditor::ResonanceSuppressorEditor (ResonanceSuppressorProcessor& p)
    : AudioProcessorEditor (&p), proc (p), graph (p)
{
    addAndMakeVisible (graph);

    setupKnob (depth,   "depth",   "DEPTH");
    setupKnob (detail,  "detail",  "DETAIL");
    setupKnob (attack,  "attack",  "ATTACK");
    setupKnob (release, "release", "RELEASE");
    setupKnob (maxCut,  "maxcut",  "MAX CUT");
    setupKnob (mix,     "mix",     "MIX");
    setupKnob (output,  "output",  "OUTPUT");

    for (auto* b : { &bypassButton, &deltaButton })
    {
        b->setClickingTogglesState (true);
        b->setColour (juce::TextButton::buttonOnColourId, Colours::reduction.darker (0.3f));
        b->setColour (juce::TextButton::buttonColourId, Colours::panel);
        addAndMakeVisible (*b);
    }

    bypassAttachment = std::make_unique<ButtonAttachment> (proc.apvts, "bypass", bypassButton);
    deltaAttachment  = std::make_unique<ButtonAttachment> (proc.apvts, "delta",  deltaButton);

    setResizable (true, true);
    setResizeLimits (700, 420, 1800, 1000);
    setSize (900, 520);

    startTimerHz (10);
}

void ResonanceSuppressorEditor::setupKnob (Knob& k, const juce::String& paramID,
                                           const juce::String& text)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 16);
    k.slider.setColour (juce::Slider::rotarySliderFillColourId, Colours::reduction);
    k.slider.setColour (juce::Slider::rotarySliderOutlineColourId, Colours::grid);
    k.slider.setColour (juce::Slider::thumbColourId, Colours::text);
    k.slider.setColour (juce::Slider::textBoxTextColourId, Colours::text);
    k.slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (k.slider);

    k.label.setText (text, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.setColour (juce::Label::textColourId, Colours::gridText);
    k.label.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (k.label);

    k.attachment = std::make_unique<SliderAttachment> (proc.apvts, paramID, k.slider);
}

void ResonanceSuppressorEditor::timerCallback()
{
    graph.setMaxCut ((float) maxCut.slider.getValue());
}

void ResonanceSuppressorEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText ("RESONANCE SUPPRESSOR", 14, 8, 300, 18,
                juce::Justification::left, false);
}

void ResonanceSuppressorEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    area.removeFromTop (24);   // Titelzeile

    auto graphArea = area.removeFromTop (juce::roundToInt (area.getHeight() * 0.58f));
    graph.setBounds (graphArea);

    area.removeFromTop (10);

    auto footer = area.removeFromBottom (34);

    auto knobRow = area;
    const int knobWidth = knobRow.getWidth() / 7;

    Knob* knobs[] = { &depth, &detail, &attack, &release, &maxCut, &mix, &output };

    for (auto* k : knobs)
    {
        auto cell = knobRow.removeFromLeft (knobWidth);
        k->label .setBounds (cell.removeFromTop (14));
        k->slider.setBounds (cell.reduced (4, 0));
    }

    bypassButton.setBounds (footer.removeFromLeft (110).reduced (2));
    footer.removeFromLeft (8);
    deltaButton .setBounds (footer.removeFromLeft (110).reduced (2));
}
