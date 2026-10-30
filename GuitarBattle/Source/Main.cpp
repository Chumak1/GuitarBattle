/*
  ==============================================================================

    This file contains the basic startup code for a JUCE application.

  ==============================================================================
*/

#include <JuceHeader.h>
#include "MainComponent.h"
#include "Audio/Vst3ScanWorker.h"
#include "UI/BrandAssets.h"
#include "UI/GameStyle.h"

//==============================================================================
class GuitarBattleApplication  : public juce::JUCEApplication
{
public:
    //==============================================================================
    GuitarBattleApplication() {}

    const juce::String getApplicationName() override       { return "FretFight"; }
    const juce::String getApplicationVersion() override    { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override             { return true; }

    //==============================================================================
    void initialise (const juce::String& commandLine) override
    {
        juce::ignoreUnused(commandLine);

        // VST3 scan helper: the VST host launches this executable with a single
        // plugin file to inspect. It must not open a window, start audio or
        // touch any user settings - it only writes the scan result and exits.
        const auto arguments = getCommandLineParameterArray();

        if (Vst3ScanWorker::isWorkerCommandLine (arguments))
        {
            Vst3ScanWorker::runAndQuit (arguments);
            return;
        }

        mainWindow.reset (new MainWindow (getApplicationName()));
    }

    void shutdown() override
    {
        // Add your application's shutdown code here..

        mainWindow = nullptr; // (deletes our window)
    }

    //==============================================================================
    void systemRequestedQuit() override
    {
        // This is called when the app is being asked to quit: you can ignore this
        // request and let the app carry on running, or call quit() to allow the app to close.
        quit();
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        juce::ignoreUnused(commandLine);
        // This application does not process arguments from a second instance.
    }

    //==============================================================================
    /*
        This class implements the desktop window that contains an instance of
        our MainComponent class.
    */
    class MainWindow    : public juce::DocumentWindow
    {
    public:
        MainWindow (juce::String name)
            : DocumentWindow (name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                                          .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setBackgroundColour (UiColors::background);
            setIcon (BrandAssets::createLogoImage (256));

            // Allow the window to cover the whole desktop, including displays
            // larger than 1920x1200, while keeping the original minimum size.
            auto* display = juce::Desktop::getInstance()
                                .getDisplays().getPrimaryDisplay();
            const int maxWidth = display != nullptr
                ? juce::jmax (1920, juce::roundToInt (display->logicalBounds.getWidth()))
                : 1920;
            const int maxHeight = display != nullptr
                ? juce::jmax (1200, juce::roundToInt (display->logicalBounds.getHeight()))
                : 1200;
            setResizeLimits (900, 800, maxWidth, maxHeight);
            setResizable (true, true);

           #if JUCE_IOS || JUCE_ANDROID
            setVisible (true);
            setFullScreen (true);
           #else
            // Size the window to the display before its peer is created so the
            // first frame is already full-screen, then let the peer take over.
            if (display != nullptr)
                setBounds (display->logicalBounds.toNearestInt());

            setVisible (true);

            // JUCE maps setFullScreen() to a maximised window on Windows, which
            // leaves the native title bar and taskbar visible. Kiosk mode
            // removes the title bar and stretches the window over the whole
            // display so the app really opens full-screen.
            juce::Desktop::getInstance().setKioskModeComponent (this, false);
           #endif
        }

        void closeButtonPressed() override
        {
            // This is called when the user tries to close this window. Here, we'll just
            // ask the app to quit when this happens, but you can change this to do
            // whatever you need.
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

        /* Note: Be careful if you override any DocumentWindow methods - the base
           class uses a lot of them, so by overriding you might break its functionality.
           It's best to do all your work in your content component instead, but if
           you really have to override any DocumentWindow methods, make sure your
           subclass also calls the superclass's method.
        */

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

//==============================================================================
// This macro generates the main() routine that launches the app.
START_JUCE_APPLICATION (GuitarBattleApplication)
