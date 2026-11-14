#pragma once

#include <JuceHeader.h>

#include "../Audio/VstHost.h"

//==============================================================================
// VST3 chooser for the main screen.
//
// The panel lists scanned effect plugins, loads the selected one into the
// shared VstHost, and exposes bypass / editor / remove. Scanning and loading
// run in the background; this component only polls the host on the message
// thread.

class VstPanel final : public juce::Component,
                       private juce::ListBoxModel,
                       private juce::Timer
{
public:
    explicit VstPanel (VstHost& hostToUse);
    ~VstPanel() override;

    // The chooser is normally a dialog; in the VST section it is embedded in
    // the hub, so the CLOSE button is not needed.
    void setEmbedded (bool shouldBeEmbedded);
    bool isEmbedded() const noexcept { return embedded; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    //==========================================================================
    // ListBoxModel

    int getNumRows() override;
    void paintListBoxItem (int rowNumber,
                           juce::Graphics& g,
                           int width,
                           int height,
                           bool rowIsSelected) override;
    void listBoxItemDoubleClicked (int row,
                                   const juce::MouseEvent& event) override;
    void selectedRowsChanged (int lastRowSelected) override;

    //==========================================================================
    // Timer

    void timerCallback() override;

    //==========================================================================
    void refreshPluginList();
    void updateControls();
    void updateStatusText();
    void startScan();
    void loadSelected();
    void toggleBypass();
    void openEditor();
    void removePlugin();

    VstHost& host;

    juce::Label loadedLabel;
    juce::Label statusLabel;
    juce::ListBox pluginList{ "VST plugins", this };

    juce::TextButton scanButton{ "RESCAN" };
    juce::TextButton loadButton{ "LOAD" };
    juce::TextButton bypassButton{ "BYPASS" };
    juce::TextButton editorButton{ "EDITOR" };
    juce::TextButton removeButton{ "REMOVE" };
    juce::TextButton closeButton{ "CLOSE" };

    juce::Array<juce::PluginDescription> types;
    int lastStateVersion = -1;
    bool embedded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VstPanel)
};
