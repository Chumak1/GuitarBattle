#include "VstPanel.h"

#include "GameStyle.h"

//==============================================================================
namespace
{
    juce::DialogWindow* findDialog (juce::Component& component)
    {
        return component.findParentComponentOfClass<juce::DialogWindow>();
    }
}

//==============================================================================
VstPanel::VstPanel (VstHost& hostToUse)
    : host (hostToUse)
{
    setSize (760, 620);

    loadedLabel.setFont (GameUi::font (14.0f, true));
    loadedLabel.setColour (juce::Label::textColourId, UiColors::text);
    loadedLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (loadedLabel);

    statusLabel.setFont (GameUi::font (12.0f));
    statusLabel.setColour (juce::Label::textColourId, UiColors::muted);
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);

    pluginList.setRowHeight (28);
    pluginList.setColour (juce::ListBox::backgroundColourId, UiColors::surface);
    pluginList.setColour (juce::ListBox::outlineColourId, UiColors::border);
    pluginList.setOutlineThickness (1);
    addAndMakeVisible (pluginList);

    auto styleButton = [this] (juce::TextButton& button,
                               const juce::String& tooltip)
    {
        button.setColour (juce::TextButton::buttonColourId, UiColors::surface2);
        button.setColour (juce::TextButton::buttonOnColourId, UiColors::accentStrong);
        button.setColour (juce::TextButton::textColourOffId, UiColors::text);
        button.setColour (juce::TextButton::textColourOnId, UiColors::onAccent);
        button.setTooltip (tooltip);
        addAndMakeVisible (button);
    };

    styleButton (scanButton, "Scan the standard VST3 folders for effect plugins");
    styleButton (loadButton, "Load the selected plugin into the monitor path");
    styleButton (bypassButton, "Process or bypass the monitored guitar");
    styleButton (editorButton, "Open the plugin editor window");
    styleButton (removeButton, "Remove the plugin and free its resources");
    styleButton (closeButton, "Close this window");

    scanButton.onClick = [this]
    {
        if (host.isScanning())
            host.cancelScan();
        else
            startScan();
    };
    loadButton.onClick = [this] { loadSelected(); };
    bypassButton.onClick = [this] { toggleBypass(); };
    editorButton.onClick = [this] { openEditor(); };
    removeButton.onClick = [this] { removePlugin(); };

    closeButton.onClick = [this]
    {
        if (auto* dialog = findDialog (*this))
            dialog->setVisible (false);
    };

    refreshPluginList();
    updateControls();
    updateStatusText();

    startTimerHz (10);
}

VstPanel::~VstPanel()
{
    stopTimer();
}

//==============================================================================
void VstPanel::paint (juce::Graphics& g)
{
    g.fillAll (UiColors::background);

    auto bounds = getLocalBounds().toFloat();
    GameUi::drawCard (g, bounds.reduced (12.0f), 20.0f, true);

    g.setColour (UiColors::accent);
    g.fillRoundedRectangle (juce::Rectangle<float> (30.0f, 29.0f, 5.0f, 36.0f),
                            2.5f);

    g.setColour (UiColors::text);
    g.setFont (GameUi::heading (25.0f));
    g.drawText ("VST PLUGIN", 48, 26, getWidth() - 200, 34,
                juce::Justification::centredLeft);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (13.0f));
    g.drawText ("The plugin processes the monitored guitar only. "
                "Note detection and scoring stay dry.",
                48, 59, getWidth() - 80, 24,
                juce::Justification::centredLeft);

    GameUi::drawPill (g,
                      juce::Rectangle<float> (static_cast<float> (getWidth()) - 158.0f,
                                              31.0f, 120.0f, 22.0f),
                      "MONITOR FX",
                      UiColors::accent,
                      true);

    g.setColour (UiColors::border);
    g.drawLine (30.0f, 96.0f,
                static_cast<float> (getWidth() - 30), 96.0f, 1.0f);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (10.5f));
    g.drawText ("VST3 effect plugins from the standard system folders. "
                "Double-click a plugin to load it.",
                30, getHeight() - 58, getWidth() - 60, 20,
                juce::Justification::centredLeft, false);
}

void VstPanel::resized()
{
    const int margin = 30;
    const int width = getWidth() - margin * 2;

    loadedLabel.setBounds (margin, 108, width, 24);

    const int listTop = 140;
    const int listBottom = getHeight() - 200;
    pluginList.setBounds (margin, listTop, width,
                          juce::jmax (80, listBottom - listTop));

    statusLabel.setBounds (margin, getHeight() - 192, width, 22);

    const int gap = 10;
    const int buttonH = 40;
    const int buttonY = getHeight() - 152;
    const int buttonW = (width - gap * 5) / 6;

    scanButton.setBounds (margin, buttonY, buttonW, buttonH);
    loadButton.setBounds (margin + (buttonW + gap), buttonY, buttonW, buttonH);
    bypassButton.setBounds (margin + (buttonW + gap) * 2, buttonY,
                            buttonW, buttonH);
    editorButton.setBounds (margin + (buttonW + gap) * 3, buttonY,
                            buttonW, buttonH);
    removeButton.setBounds (margin + (buttonW + gap) * 4, buttonY,
                            buttonW, buttonH);
    closeButton.setBounds (margin + (buttonW + gap) * 5, buttonY,
                           buttonW, buttonH);
}

//==============================================================================
int VstPanel::getNumRows()
{
    return types.size();
}

void VstPanel::paintListBoxItem (int rowNumber,
                                 juce::Graphics& g,
                                 int width,
                                 int height,
                                 bool rowIsSelected)
{
    if (! juce::isPositiveAndBelow (rowNumber, types.size()))
        return;

    const auto& description = types.getReference (rowNumber);

    if (rowIsSelected)
        g.fillAll (UiColors::accent.withAlpha (0.18f));

    const bool isLoaded = host.isPluginLoaded()
        && description.name == host.getLoadedPluginName();

    if (isLoaded)
    {
        g.setColour (UiColors::accent);
        g.fillRoundedRectangle (6.0f, static_cast<float> (height) * 0.5f - 3.0f,
                                6.0f, 6.0f, 3.0f);
    }

    g.setColour (isLoaded ? UiColors::accent : UiColors::text);
    g.setFont (GameUi::font (13.0f, true));
    g.drawText (description.name, 20, 0, width - 220, height,
                juce::Justification::centredLeft, true);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (11.0f));
    g.drawText (description.manufacturerName, width - 200, 0, 190, height,
                juce::Justification::centredRight, true);
}

void VstPanel::listBoxItemDoubleClicked (int row,
                                         const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow (row, types.size()))
    {
        pluginList.selectRow (row);
        loadSelected();
    }
}

void VstPanel::selectedRowsChanged (int)
{
    updateControls();
}

//==============================================================================
void VstPanel::timerCallback()
{
    const int version = host.getStateVersion();

    if (version != lastStateVersion)
    {
        lastStateVersion = version;
        refreshPluginList();
        updateControls();
    }

    updateStatusText();
}

void VstPanel::refreshPluginList()
{
    // Keep the selection on the same plugin across rescans and loads.
    juce::String selectedIdentifier;

    const int selectedRow = pluginList.getSelectedRow();

    if (juce::isPositiveAndBelow (selectedRow, types.size()))
        selectedIdentifier = types.getReference (selectedRow).createIdentifierString();

    types = host.getScannedPlugins();
    pluginList.updateContent();

    int newSelection = -1;

    if (selectedIdentifier.isNotEmpty())
    {
        for (int i = 0; i < types.size(); ++i)
        {
            if (types.getReference (i).createIdentifierString()
                == selectedIdentifier)
            {
                newSelection = i;
                break;
            }
        }
    }

    if (newSelection >= 0)
        pluginList.selectRow (newSelection, true, true);
    else
        pluginList.deselectAllRows();
}

void VstPanel::updateControls()
{
    const bool hasSelection = pluginList.getSelectedRow() >= 0;
    const bool scanning = host.isScanning();
    const bool loaded = host.isPluginLoaded();

    scanButton.setEnabled (true);
    scanButton.setButtonText (scanning ? "CANCEL" : "RESCAN");
    loadButton.setEnabled (hasSelection && ! scanning);
    bypassButton.setEnabled (loaded);
    editorButton.setEnabled (loaded);
    removeButton.setEnabled (loaded);

    bypassButton.setToggleState (host.isBypassed(), juce::dontSendNotification);

    const auto name = host.getLoadedPluginName();

    if (loaded)
    {
        loadedLabel.setColour (juce::Label::textColourId, UiColors::accent);
        loadedLabel.setText ("LOADED: " + name.toUpperCase(),
                             juce::dontSendNotification);
    }
    else
    {
        loadedLabel.setColour (juce::Label::textColourId, UiColors::muted);
        loadedLabel.setText ("NO PLUGIN LOADED", juce::dontSendNotification);
    }
}

void VstPanel::updateStatusText()
{
    if (host.isScanning())
    {
        const int percent = juce::roundToInt (host.getScanProgress() * 100.0f);
        statusLabel.setText ("Scanning VST3 folders... "
                                 + juce::String (percent) + "%",
                             juce::dontSendNotification);
        return;
    }

    statusLabel.setText (host.getLastStatusMessage(),
                         juce::dontSendNotification);
}

//==============================================================================
void VstPanel::startScan()
{
    host.scanPlugins();
    updateControls();
    updateStatusText();
}

void VstPanel::loadSelected()
{
    const int row = pluginList.getSelectedRow();

    if (! juce::isPositiveAndBelow (row, types.size()))
        return;

    host.loadPlugin (types.getReference (row));
    updateControls();
    updateStatusText();
}

void VstPanel::toggleBypass()
{
    host.setBypassed (! host.isBypassed());
    updateControls();
}

void VstPanel::openEditor()
{
    host.showEditor();
    updateStatusText();
}

void VstPanel::removePlugin()
{
    host.unloadPlugin();
    updateControls();
    updateStatusText();
}
