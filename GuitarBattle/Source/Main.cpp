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
#if JUCE_WINDOWS && defined(_DEBUG)
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
#if JUCE_WINDOWS && defined(_DEBUG)
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
    // Applies the 16:9 ratio to the *content* area rather than to the whole
    // window. With a native title bar the two differ by the frame, so a plain
    // setFixedAspectRatio() leaves the client area slightly short.
    class ContentAspectConstrainer final : public juce::ComponentBoundsConstrainer
    {
    public:
        void setFrameSize (int width, int height) noexcept
        {
            frameW = width;
            frameH = height;
        }

        void checkBounds (juce::Rectangle<int>& bounds,
                          const juce::Rectangle<int>& previousBounds,
                          const juce::Rectangle<int>& limits,
                          bool isStretchingTop,
                          bool isStretchingLeft,
                          bool isStretchingBottom,
                          bool isStretchingRight) override
        {
            juce::ComponentBoundsConstrainer::checkBounds (bounds, previousBounds, limits,
                                                           isStretchingTop, isStretchingLeft,
                                                           isStretchingBottom, isStretchingRight);

            if (frameW <= 0 || frameH <= 0)
                return;

            int contentW = bounds.getWidth() - frameW;
            int contentH = bounds.getHeight() - frameH;

            if (contentW <= 0 || contentH <= 0)
                return;

            if (contentW * 9 > contentH * 16)
                contentW = contentH * 16 / 9;
            else
                contentH = contentW * 9 / 16;

            bounds.setSize (contentW + frameW, contentH + frameH);
        }

    private:
        int frameW = 0;
        int frameH = 0;
    };

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
                              DocumentWindow::minimiseButton
                                  | DocumentWindow::closeButton)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setBackgroundColour (UiColors::background);
            setIcon (BrandAssets::createLogoImage (256));

            // FretFight runs in a normal 16:9 window, never full-screen/kiosk.
            // The window can be resized, but the constrainer keeps the content
            // area at 16:9 so the gameplay field matches the artwork.
            auto* display = juce::Desktop::getInstance()
                                .getDisplays().getPrimaryDisplay();
            const auto userArea = display != nullptr
                ? display->userBounds.toNearestInt()
                : juce::Rectangle<int> (0, 0, 1920, 1080);

            const int maxWidth = juce::jmax (1280, userArea.getWidth());
            const int maxHeight = juce::jmax (720, userArea.getHeight());
            setResizeLimits (1280, 720, maxWidth, maxHeight);

            setConstrainer (&constrainer);
            setResizable (true, true);

            int windowW = juce::jmin (1600, userArea.getWidth());
            int windowH = juce::roundToInt (static_cast<double> (windowW) * 9.0 / 16.0);

            if (windowH > userArea.getHeight())
            {
                windowH = userArea.getHeight();
                windowW = juce::roundToInt (static_cast<double> (windowH) * 16.0 / 9.0);
            }

            setBounds (userArea.getCentreX() - windowW / 2,
                       userArea.getCentreY() - windowH / 2,
                       windowW, windowH);

           #if JUCE_IOS || JUCE_ANDROID
            setFullScreen (true);
           #endif

            setVisible (true);

            // The native frame is only measurable once the peer exists. Measure
            // it, then size the whole window so the content is exactly 16:9.
            if (auto* content = getContentComponent())
            {
                const int frameW = juce::jmax (0, getWidth() - content->getWidth());
                const int frameH = juce::jmax (0, getHeight() - content->getHeight());

                constrainer.setFrameSize (frameW, frameH);

                setBounds (userArea.getCentreX() - (windowW + frameW) / 2,
                           userArea.getCentreY() - (windowH + frameH) / 2,
                           windowW + frameW, windowH + frameH);
            }
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
        ContentAspectConstrainer constrainer;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

//==============================================================================
// This macro generates the main() routine that launches the app.
START_JUCE_APPLICATION (GuitarBattleApplication)
