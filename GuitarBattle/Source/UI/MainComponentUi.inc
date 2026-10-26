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
    juce::Component::SafePointer<juce::DialogWindow> accountDialog;
    void openAccountWindow();
