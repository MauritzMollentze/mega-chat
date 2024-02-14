/**
 * @file
 * @brief 
 *
 */

#include "mclc_no_interactive.h"

#include "mclc_chat_and_call_actions.h"
#include "mclc_globals.h"
#include "mclc_prompt.h"

#include <cassert>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>

namespace mclc
{

int noInteractiveCommand(int argc, char* argv[])
{
    if (argc == 0)
    {
        return 0; // nothing to do
    }
    auto command = clc_noint::commandFactory(argv[0]);
    if (!command)
    {
        std::cerr << "Invalid command '" << argv[0] << "'. The available options are:\n";
        for (auto [k, _] : clc_noint::strToCommands)
        {
            std::cerr << "    + " + std::string(k) + "\n";
        }
        return 1;
    }
    return std::visit(
        [&argc, &argv](auto&& cmd)
        {
            return cmd(argc, argv);
        },
        *command);
}

namespace clc_noint
{

JoinCallViaMeetingLink::JoinCallViaMeetingLink():
    mDesc{"Options for JoinCallViaMeetingLink command:"}
{
    // clang-format off
    mDesc.add_options()
        ("help,h", "Show help message")
        ((OPT_DEBUG + std::string(",d")).c_str(), "If specify, log messages will be printed to the file ./<pid>/joinCall.log")
        (OPT_VIDEO, po::value<std::string>()->default_value("no"), "[yes|no] If yes, tries to call with video")
        (OPT_AUDIO, po::value<std::string>()->default_value("no"), "[yes|no] If yes, tries to call with audio")
        (OPT_VIDEO_DEV, po::value<std::string>(), "The input video device name to use in the call")
        (OPT_WAIT, po::value<std::string>()->default_value("40"), "Time to stay in the call. 0 means never hang up.")
        ((OPT_EMAIL + std::string(",e")).c_str(), po::value<std::string>()->required(), "User's email to login")
        ((OPT_PASS + std::string(",p")).c_str(), po::value<std::string>()->required(), "User's password to login")
        ((OPT_URL + std::string(",l")).c_str(), po::value<std::string>()->required(), "Link to the chat to join and receive the call")
        ;
    // clang-format on
}

void JoinCallViaMeetingLink::validateInput(const po::variables_map& variablesMap)
{
    const std::vector<const char*> mandatoryFields{OPT_EMAIL, OPT_PASS, OPT_URL};
    for (auto& field: mandatoryFields)
    {
        if (variablesMap.count(field) == 0)
        {
            throw std::logic_error("Missing required option " + std::string(field));
        }
    }

    const std::vector<const char*> yesNoFields{OPT_AUDIO, OPT_VIDEO};
    for (auto& field: yesNoFields)
    {
        const std::string value = variablesMap[field].as<std::string>();
        if (value != "yes" && value != "no")
        {
            throw std::logic_error("The " + std::string(field) + " option must be yes or no");
        }
    }
}

void JoinCallViaMeetingLink::runCommand(const po::variables_map& variablesMap)
{
    clc_prompt::setprompt(clc_prompt::COMMAND);
    if (variablesMap.count(OPT_DEBUG) != 0)
    {
        clc_prompt::process_line("debug -pid -console warning -file all joinCall.log");
    }
    auto email = variablesMap[OPT_EMAIL].as<std::string>();
    auto password = variablesMap[OPT_PASS].as<std::string>();
    if (!clc_ccactions::login(email.c_str(), password.c_str()))
    {
        throw std::logic_error("Unable to login");
    }
    std::ostringstream joinCommand;
    joinCommand << "joinCallViaMeetingLink";
    if (variablesMap[OPT_VIDEO].as<std::string>() == "no")
    {
        joinCommand << " -novideo";
    }
    if (variablesMap[OPT_AUDIO].as<std::string>() == "no")
    {
        joinCommand << " -noaudio";
    }
    joinCommand << " -wait " << variablesMap[OPT_WAIT].as<std::string>();
    if (variablesMap.count("video-in-device") != 0)
    {
        joinCommand << " -videoInputDevice " << variablesMap[OPT_VIDEO_DEV].as<std::string>();
    }
    joinCommand << " " << variablesMap[OPT_URL].as<std::string>();

    clc_prompt::process_line(joinCommand.str().c_str());
    return;
}

int JoinCallViaMeetingLink::operator()(int argc, char* argv[])
{
    po::variables_map variablesMap;
    try
    {
        po::store(po::parse_command_line(argc, argv, mDesc), variablesMap);
        po::notify(variablesMap);

        if (variablesMap.count("help"))
        {
            std::cout << mDesc << "\n";
            return 0;
        }
        validateInput(variablesMap);
        runCommand(variablesMap);
    }
    catch (std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    catch (...)
    {
        std::cerr << "Unknown error.\n";
        return 1;
    }
    return 0;
}

Help::Help()
{
    mHelpMsg = R"(
You are using the megaclc app. This app offers a cli oriented client to consume
the MEGAChat API.

NOTE: You can safely kill the execution of the app with a SIGINT (C-c) or a
SIGTERM signal (kill <pid>).

To use it in interactive mode you can run the binary without any argument. This
initializes a terminal to run commands interactively. If you type "help" in the
terminal you will see the list of available commands.

To run non interactive commands you must provide the name of the utility you
want to run after the name of the megaclc binary and also the options (if it
accepts any). For example:

    megaclc joinCallViaMeetingLink -h

Currently, the available utilities are:

)";
    for (auto [k, _] : strToCommands)
    {
        mHelpMsg += "    + " + std::string(k) + "\n";
    }
}

int Help::operator()(int argc, char*[])
{
    if (argc > 1)
    {
        std::cerr << "Help command does not accept additional argumetns\n";
        return 1;
    }
    std::cout << mHelpMsg;
    return 0;
}

int Test::operator()(int argc, char* argv[])
{
    po::options_description desc("Test command for debugging purposes");
    desc.add_options()
        ("url,l", po::value<std::string>()->required(), "Link to the chat to join and receive the call")
        ;
    po::variables_map variablesMap;
    try{
        po::store(po::parse_command_line(argc, argv, desc), variablesMap);
        po::notify(variablesMap);
        std::cout << variablesMap["url"].as<std::string>() << "\n";
    }
    catch (...)
    {
        return 1;
    }
    return 0;
}

std::optional<AvailableCommands> commandFactory(const std::string_view& commandName)
{
    if (auto it = strToCommands.find(commandName); it != strToCommands.end())
    {
        return it->second();
    }
    return {};
}
}

} // namespace mclc
