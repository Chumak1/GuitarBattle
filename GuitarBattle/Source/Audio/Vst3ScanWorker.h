#pragma once

#include <JuceHeader.h>

//==============================================================================
// Out-of-process VST3 scanning.
//
// JUCE's VST3 format requires plugin initialisation to run on the message
// thread (the VST3 spec demands it) and a broken plugin can crash the whole
// process while it is being inspected. The scanner therefore re-launches this
// same executable in "worker" mode: the helper scans exactly one .vst3 file on
// its own message thread and writes the discovered descriptions to an XML
// file. The main process stays responsive, and a hung or crashing plugin only
// takes the helper down with it.
//
// The helper is started by VstHost with:
//     GuitarBattle.exe --vst3-scan-file=<plugin> --vst3-scan-out=<xml>
// and exits as soon as the result has been written.
namespace Vst3ScanWorker
{
    inline constexpr const char* fileArgument   = "--vst3-scan-file=";
    inline constexpr const char* outputArgument = "--vst3-scan-out=";

    inline bool isWorkerCommandLine (const juce::StringArray& arguments)
    {
        for (const auto& argument : arguments)
            if (argument.startsWith (fileArgument))
                return true;

        return false;
    }

    inline juce::String argumentValue (const juce::StringArray& arguments,
                                       const char* prefix)
    {
        for (const auto& argument : arguments)
            if (argument.startsWith (prefix))
                return argument.substring (juce::String (prefix).length());

        return {};
    }

    // Runs on the helper's message thread. The result file is only written
    // when the scan returns, so a crash leaves no file behind and the parent
    // can tell a crash apart from "this plugin contains nothing".
    inline void run (const juce::StringArray& arguments)
    {
        const auto pluginFile = argumentValue (arguments, fileArgument);
        const juce::File outputFile (argumentValue (arguments, outputArgument));

        if (pluginFile.isEmpty() || outputFile.getFullPathName().isEmpty())
            return;

        juce::OwnedArray<juce::PluginDescription> found;

       #if JUCE_PLUGINHOST_VST3
        juce::VST3PluginFormat format;
        format.findAllTypesForFile (found, pluginFile);
       #endif

        juce::XmlElement list ("LIST");

        for (const auto* description : found)
            if (auto xml = description->createXml())
                list.addChildElement (xml.release());

        outputFile.getParentDirectory().createDirectory();
        outputFile.replaceWithText (list.toString (juce::XmlElement::TextFormat()
                                                       .singleLine()),
                                    false, false, "\n");
    }

    // Defers the scan until the helper's message loop is running (some plugins
    // expect a live event loop during initialisation) and quits afterwards.
    inline void runAndQuit (const juce::StringArray& arguments)
    {
        juce::MessageManager::callAsync ([arguments]
        {
            run (arguments);
            juce::JUCEApplicationBase::quit();
        });
    }
}
