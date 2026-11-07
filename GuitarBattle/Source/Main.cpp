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

#if JUCE_WINDOWS
 #include <crtdbg.h>
 #include <windows.h>
#endif

//==============================================================================
// Diagnostic log for debug-build assertions.
//
// A Debug build breaks on a failed debug assertion (for example the MSVC STL
// "Invalid memory order" check). Without a debugger the only trace is the
// Windows crash report, which has no stack. This hook writes the assertion
// text and the return addresses to %APPDATA%\GuitarBattle\assert.log before
// the break, so the caller can be resolved with the PDB. Release builds are
// not affected (the CRT hook is compiled in but no assertion fires there).
namespace CrashDiagnostics
{
#if JUCE_WINDOWS
    static juce::File assertionLogFile()
    {
        auto directory = juce::File::getSpecialLocation (
                             juce::File::userApplicationDataDirectory)
                             .getChildFile ("GuitarBattle");

        directory.createDirectory();

        return directory.getChildFile ("assert.log");
    }

    static int __cdecl assertionReportHook (int reportType, char* message,
                                            int* returnValue)
    {
        juce::String text;
        text << "=== report " << reportType << " ===\n";
        text << (message != nullptr ? message : "(no message)") << "\n";

        void* frames[48];
        const USHORT count = RtlCaptureStackBackTrace (2, 48, frames, nullptr);

        text << "moduleBase=0x"
             << juce::String::toHexString (
                    static_cast<juce::int64> (
                        reinterpret_cast<juce::pointer_sized_int> (
                            GetModuleHandleW (nullptr))))
             << "\n";

        for (USHORT i = 0; i < count; ++i)
            text << "frame" << static_cast<int> (i) << "=0x"
                 << juce::String::toHexString (
                        static_cast<juce::int64> (
                            reinterpret_cast<juce::pointer_sized_int> (frames[i])))
                 << "\n";

        assertionLogFile().appendText (text);

        if (returnValue != nullptr)
            *returnValue = 0;

        // Handled: skip the modal assertion dialog. The assertion itself still
        // stops the debug build, but the log is already written.
        return TRUE;
    }
#endif

    static void install()
    {
#if JUCE_WINDOWS
        _CrtSetReportHook2 (_CRT_RPTHOOK_INSTALL, assertionReportHook);
#endif
    }
}

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

        CrashDiagnostics::install();

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
            //
            // Applied on the next message-loop iteration and only when the
            // window already owns a peer and sits on a display: Desktop::
            // setKioskModeComponent() asserts (and stops the app in a Debug
            // build) when either is not true yet, which happens when the window
            // is created while the session is locked, the display list is still
            // empty, or the app is started from an installer.
            juce::Component::SafePointer<MainWindow> safeWindow (this);

            juce::MessageManager::callAsync ([safeWindow]
            {
                auto* window = safeWindow.getComponent();

                if (window == nullptr || window->getPeer() == nullptr)
                    return;

                auto& desktop = juce::Desktop::getInstance();

                if (desktop.getDisplays().getDisplayForRect (window->getScreenBounds()) == nullptr)
                    return;

                desktop.setKioskModeComponent (window, false);
            });
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
