//==============================================================================
// Frontend extension point
//==============================================================================
//
// This file is included INSIDE the private section of MainComponent. It is the
// frontend agent's workspace: add new UI widgets, frontend-only state and UI
// helper declarations here instead of editing MainComponent.h.
//
// Implement the methods in UI/MainComponentUI.cpp. Do not add audio, network
// or gameplay logic here.
//
// The file must contain member declarations only: no class/namespace wrapper,
// no #pragma once and no #include of this file anywhere else.
//
// Existing UI setup lives in initialiseUi() (implemented in
// UI/MainComponentUI.cpp). Add new widgets/state/helper declarations below,
// then implement them in that file.

    void initialiseUi();

    // --- Frontend additions -------------------------------------------------
    //
    // Example:
    //     juce::Label newFeatureLabel;
    //     juce::TextButton newFeatureButton { "NEW FEATURE" };
    //     bool newFeatureOpen = false;
    //     void initialiseNewFeature();

    // --- Account window ------------------------------------------------------
    // The username is shown as a plain label; SETTINGS opens the personal
    // account window with rating summary, global top-50, attempt history,
    // profile/password forms and sign-out.
    juce::TextButton accountSettingsButton;

    // Opens the public donation page in the default browser. Lives in the
    // song-select header next to SETTINGS / AUDIO SETTINGS / EXIT.
    juce::TextButton supportButton;

    // Opens the in-app feedback form. The message is posted to /feedback and
    // emailed to the project address.
    juce::TextButton feedbackButton;
    juce::Component::SafePointer<juce::DialogWindow> accountDialog;
    // Set while the PROFILE dialog is open: re-reads its character catalogue
    // after a server-side unlock. Assigned in MainComponentUI.cpp.
    std::function<void()> refreshOpenAccountCharacters;
    void openAccountWindow();

    // --- VST monitor plugin ----------------------------------------------
    // The button sits in the header next to SETTINGS / AUDIO SETTINGS. It
    // opens a non-modal window so the plugin editor can stay interactive.
    juce::TextButton vstButton{ "VST" };
    std::unique_ptr<juce::DialogWindow> vstDialogWindow;
    void openVstPanel();
    void updateVstButton();
