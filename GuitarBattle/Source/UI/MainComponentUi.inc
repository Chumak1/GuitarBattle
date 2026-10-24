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
    // Clicking the signed-in username opens a separate account window with the
    // same structure as the web account page: rating summary, global top-50,
    // attempt history, profile/password forms and sign-out.
    juce::TextButton accountMenuButton;
    juce::Component::SafePointer<juce::DialogWindow> accountDialog;
    void openAccountWindow();
