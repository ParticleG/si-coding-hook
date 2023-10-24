#include <chrono>
//#include <ranges>
#include <regex>

#include <magic_enum.hpp>
#include <nlohmann/json.hpp>
#include <httplib.h>

#include <types/Configurator.h>
#include <types/RegistryMonitor.h>
#include <types/UserAction.h>
#include <types/WindowInterceptor.h>
#include <utils/crypto.h>
#include <utils/inputbox.h>
#include <utils/logger.h>
#include <utils/system.h>

using namespace magic_enum;
using namespace std;
using namespace std::ranges;
using namespace types;
using namespace utils;

namespace {
    const regex editorInfoRegex(
            R"regex(^cursor="(.*?)";path="(.*?)";project="(.*?)";tabs="(.*?)";type="(.*?)";version="(.*?)";symbols="(.*?)";prefix="(.*?)";suffix="(.*?)"$)regex");
    const regex cursorRegex(
            R"regex(^lnFirst="(.*?)";ichFirst="(.*?)";lnLast="(.*?)";ichLim="(.*?)";fExtended="(.*?)";fRect="(.*?)"$)regex");

    optional<string> generateCompletion(const string &editorInfo, const string &projectId) {
        nlohmann::json requestBody = {
                {"info",      crypto::encode(editorInfo, crypto::Encoding::Base64)},
                {"projectId", projectId},
        };
        auto client = httplib::Client("http://localhost:3000");
        client.set_connection_timeout(10);
        client.set_read_timeout(10);
        client.set_write_timeout(10);
        if (auto res = client.Post(
                "/generate",
                requestBody.dump(),
                "application/json"
        )) {
            const auto responseBody = nlohmann::json::parse(res->body);
            const auto result = responseBody["result"].get<string>();
            const auto &contents = responseBody["contents"];
            if (result == "success" && contents.is_array() && !contents.empty()) {
                return crypto::decode(contents[0].get<string>(), crypto::Encoding::Base64);
            }
            logger::log(format("HTTP result: {}", result));
        } else {
            logger::log(format("HTTP error: {}", httplib::to_string(res.error())));
        }
        return nullopt;
    }
}

RegistryMonitor::RegistryMonitor() {
    thread([this] {
        while (this->_isRunning.load()) {
            try {
                const auto editorInfoString = system::getRegValue(_subKey, "editorInfo");

//                logger::log(editorInfoString);

                smatch editorInfoRegexResults;
                if (!regex_match(editorInfoString, editorInfoRegexResults, editorInfoRegex) ||
                    editorInfoRegexResults.size() != 10) {
                    logger::log("Invalid editorInfoString");
                    continue;
                }

                const auto projectFolder = regex_replace(editorInfoRegexResults[3].str(), regex(R"(\\\\)"), "/");

                system::deleteRegValue(_subKey, "editorInfo");

                _retrieveProjectId(projectFolder);

                _retrieveCompletion(editorInfoString);

                // TODO: Finish completionType
                /*nlohmann::json editorInfo = {
                        {"cursor",          nlohmann::json::object()},
                        {"currentFilePath", regex_replace(editorInfoRegexResults[2].str(), regex(R"(\\\\)"), "/")},
                        {"projectFolder",   regex_replace(editorInfoRegexResults[3].str(), regex(R"(\\\\)"), "/")},
                        {"openedTabs",      nlohmann::json::array()},
                        {"completionType",  stoi(editorInfoRegexResults[5].str()) > 0 ? "snippet" : "line"},
                        {"version",         editorInfoRegexResults[6].str()},
                        {"symbols",         nlohmann::json::array()},
                        {"prefix",          editorInfoRegexResults[8].str()},
                        {"suffix",          editorInfoRegexResults[9].str()},
                };

                {
                    const auto cursorString = regex_replace(editorInfoRegexResults[1].str(), regex(R"(\\)"), "");
                    smatch cursorRegexResults;
                    if (!regex_match(cursorString, cursorRegexResults, cursorRegex) ||
                        cursorRegexResults.size() != 7) {
                        logger::log("Invalid cursorString");
                        continue;
                    }
                    editorInfo["cursor"] = {
                            {"startLine",      cursorRegexResults[1].str()},
                            {"startCharacter", cursorRegexResults[2].str()},
                            {"endLine",        cursorRegexResults[3].str()},
                            {"endCharacter",   cursorRegexResults[4].str()},
                    };
                }

                {
                    const auto symbolString = editorInfoRegexResults[7].str();
                    editorInfo["symbols"] = nlohmann::json::array();
                    if (symbolString.length() > 2) {
                        for (const auto symbol: views::split(symbolString.substr(1, symbolString.length() - 1), "||")) {
                            const auto symbolComponents = views::split(symbol, "|")
                                                          | views::transform(
                                    [](auto &&rng) { return string(&*rng.begin(), ranges::distance(rng)); })
                                                          | to<vector>();

                            editorInfo["symbols"].push_back(
                                    {
                                            {"name",      symbolComponents[0]},
                                            {"path",      symbolComponents[1]},
                                            {"startLine", symbolComponents[2]},
                                            {"endLine",   symbolComponents[3]},
                                    }
                            );
                        }
                    }
                }

                {
                    const string tabsString = editorInfoRegexResults[4].str();
                    auto searchStart(tabsString.cbegin());
                    smatch tabsRegexResults;
                    while (regex_search(
                            searchStart,
                            tabsString.cend(),
                            tabsRegexResults,
                            regex(R"regex(.*?\.([ch]))regex")
                    )) {
                        editorInfo["openedTabs"].push_back(tabsRegexResults[0].str());
                        searchStart = tabsRegexResults.suffix().first;
                    }
                }

                logger::log(editorInfo.dump());*/

            } catch (runtime_error &e) {
            } catch (exception &e) {
                logger::log(e.what());
            } catch (...) {
                logger::log("Unknown exception.");
            }
            this_thread::sleep_for(chrono::milliseconds(5));
        }
    }).detach();
}

RegistryMonitor::~RegistryMonitor() {
    this->_isRunning.store(false);
}

void RegistryMonitor::acceptByTab(unsigned int) {
    _justInserted = true;
    const auto completion = _completionCache.reset();
    if (!completion.empty()) {
        WindowInterceptor::GetInstance()->sendAcceptCompletion();
        thread(&RegistryMonitor::_reactToCompletion, this, completion).detach();
        logger::log("Accepted completion");
    }
}

void RegistryMonitor::cancelByCursorNavigate(CursorPosition, CursorPosition) {
    cancelByKeycodeNavigate(-1);
}

void RegistryMonitor::cancelByDeleteBackward(CursorPosition oldPosition, CursorPosition newPosition) {
    if (oldPosition.line == newPosition.line) {
        if (!_completionCache.empty()) {
            const auto previousCache = _completionCache.getPrevious();
            if (previousCache.has_value()) {
                system::setRegValue(_subKey, "cancelType", to_string(enum_integer(UserAction::DeleteBackward)));
                WindowInterceptor::GetInstance()->sendCancelCompletion();
                system::setRegValue(_subKey, "completionGenerated", previousCache.value());
                WindowInterceptor::GetInstance()->sendInsertCompletion();
                logger::log("Insert previous cached completion");
            } else {
                // out of cache
                system::setRegValue(_subKey, "cancelType", to_string(enum_integer(UserAction::DeleteBackward)));
                WindowInterceptor::GetInstance()->sendCancelCompletion();
                logger::log("Canceled by delete backward.");
                _completionCache.reset();
            }
        }
        try {
            if (_completionCache.previous()) {

            } else {
            }
        } catch (runtime_error &e) {
            logger::log(e.what());
        }
    } else {
        cancelByModifyLine(enum_integer(Key::BackSpace));
    }
}

void RegistryMonitor::cancelByKeycodeNavigate(unsigned int) {
    {
        shared_lock<shared_mutex> lock(_completionMutex);
        if (_originalCompletion.empty()) {
            return;
        }
    }
    try {
        system::setRegValue(_subKey, "cancelType", to_string(enum_integer(UserAction::Navigate)));
        WindowInterceptor::GetInstance()->sendCancelCompletion();
        logger::log("Canceled by navigate.");
        unique_lock<shared_mutex> lock(_completionMutex);
        _originalCompletion.clear();
    } catch (runtime_error &e) {
        logger::log(e.what());
    }
}

void RegistryMonitor::cancelByModifyLine(unsigned int keycode) {
    const auto windowInterceptor = WindowInterceptor::GetInstance();
    _justInserted = false;
    // TODO: Check performance impact
    unique_lock<shared_mutex> lock(_completionMutex);
    if (!_originalCompletion.empty()) {
        try {
            system::setRegValue(_subKey, "cancelType", to_string(enum_integer(UserAction::ModifyLine)));
            windowInterceptor->sendCancelCompletion();
            _originalCompletion.clear();
            if (keycode == enum_integer(Key::BackSpace)) {
                logger::log("Canceled by backspace");
            } else {
                logger::log("Canceled by enter");
            }
        } catch (runtime_error &e) {
            logger::log(e.what());
        }
    }
    if (keycode != enum_integer(Key::BackSpace)) {
        windowInterceptor->sendRetrieveInfo();
    }
}

void RegistryMonitor::cancelBySave() {
    // TODO: Check performance impact
    unique_lock<shared_mutex> lock(_completionMutex);
    if (!_originalCompletion.empty()) {
        const auto windowInterceptor = WindowInterceptor::GetInstance();
        system::setRegValue(_subKey, "cancelType", to_string(enum_integer(UserAction::Navigate)));
        windowInterceptor->sendCancelCompletion();
        _originalCompletion.clear();
        logger::log("Canceled by save.");
        windowInterceptor->sendSave();
    }
}

void RegistryMonitor::cancelByUndo() {
    const auto windowInterceptor = WindowInterceptor::GetInstance();
    // TODO: Check performance impact
    unique_lock<shared_mutex> lock(_completionMutex);
    if (_justInserted.load()) {
        _justInserted = false;
        windowInterceptor->sendUndo();
        windowInterceptor->sendUndo();
    } else if (!_originalCompletion.empty()) {
        _originalCompletion.clear();
        windowInterceptor->sendUndo();
        logger::log(("Canceled by undo"));
    }
}

void RegistryMonitor::retrieveEditorInfo(unsigned int keycode) {
    const auto windowInterceptor = WindowInterceptor::GetInstance();
    _justInserted = false;
    logger::log(format(
            "RegistryMonitor::retrieveEditorInfo: (keycode: {}, _originalCompletion: {})",
            keycode,
            _originalCompletion
    ));
    if (_originalCompletion.empty()) {
        windowInterceptor->sendRetrieveInfo();
        logger::log(format("Retrieving editor info... (keycode: {})", keycode));
        return;
    }

    if (_currentIndex < _originalCompletion.length() - 1) {
        if (keycode == _originalCompletion[_currentIndex]) {
            _currentIndex++;
            system::setRegValue(_subKey, "cancelType", to_string(enum_integer(UserAction::DeleteBackward)));
            windowInterceptor->sendCancelCompletion();
            logger::log("Canceled due to update cache");
            system::setRegValue(
                    _subKey,
                    "completionGenerated",
                    _originalCompletion[0] + _originalCompletion.substr(_currentIndex)
            );
            WindowInterceptor::GetInstance()->sendInsertCompletion();
            logger::log(format("Cache Completion: '{}'",
                               _originalCompletion[0] + _originalCompletion.substr(_currentIndex)));
        } else {
            _originalCompletion.clear();
            _currentIndex = 1;
            system::setRegValue(_subKey, "cancelType", to_string(enum_integer(UserAction::DeleteBackward)));
            windowInterceptor->sendCancelCompletion();
            logger::log("Canceled due to cache miss");
            windowInterceptor->sendRetrieveInfo();
            logger::log(format("Retrieving editor info... (keycode: {})", keycode));
        }
    } else {
        acceptByTab(keycode);
    }

    try {

    } catch (exception &e) {
        logger::log(e.what());
        thread(&RegistryMonitor::_reactToCompletion, this, _originalCompletion).detach();
        _originalCompletion.clear();
        _currentIndex = 1;
    }
}

void RegistryMonitor::_reactToCompletion(string completion) {
    try {
        nlohmann::json requestBody;
        {
            const auto isSnippet = completion[0] == '1';
            auto lines = 1;
            if (isSnippet) {
                auto pos = completion.find(R"(\n)", 0);
                while (pos != string::npos) {
                    ++lines;
                    pos = completion.find(R"(\n)", pos + 1);
                }
            }
            requestBody = {
                    {"code_line",   lines},
                    {"mode",        isSnippet},
                    {"project_id",  _projectId},
                    {"tab_output",  true},
                    {"total_lines", lines},
                    {"text_length", completion.length() - 1},
                    {"username",    Configurator::GetInstance()->username()},
                    {"version",     "SI-0.6.0"},
            };
            logger::log(requestBody.dump());
        }
        auto client = httplib::Client("http://10.113.10.68:4322");
        client.set_connection_timeout(3);
        client.Post("/code/statistical", requestBody.dump(), "application/json");
    } catch (...) {}
}

void RegistryMonitor::_retrieveCompletion(const string &editorInfoString) {
    _lastTriggerTime = chrono::high_resolution_clock::now();
    thread([this, editorInfoString, currentTriggerName = _lastTriggerTime.load()] {
        // const auto completionGenerated = generateCompletion(editorInfoString, _projectId);
        const optional<std::string> completionGenerated = "0Generated";
        if (completionGenerated.has_value() && currentTriggerName == _lastTriggerTime.load()) {
            try {
                unique_lock<shared_mutex> lock(_completionMutex);
                _originalCompletion = completionGenerated.value();
                _currentIndex = 1;
                system::setRegValue(_subKey, "completionGenerated", completionGenerated.value());
            } catch (runtime_error &e) {
                logger::log(e.what());
            }
            WindowInterceptor::GetInstance()->sendInsertCompletion();
            logger::log("Inserted completion");
        }
    }).detach();
}

void RegistryMonitor::_retrieveProjectId(const string &projectFolder) {
    const auto currentProjectHash = crypto::sha1(projectFolder);
    if (_projectHash != currentProjectHash) {
        _projectId.clear();
        _projectHash = currentProjectHash;
    }

    if (_projectId.empty()) {
        const auto projectListKey = _subKey + "\\Project List";
        while (_projectId.empty()) {
            try {
                _projectId = system::getRegValue(projectListKey, _projectHash);
            } catch (...) {
                _projectId = InputBox("Please input current project's iSoft ID", "Input Project ID");
                if (_projectId.empty()) {
                    logger::error("Project ID is empty.");
                } else {
                    system::setRegValue(projectListKey, _projectHash, _projectId);
                }
            }
        }
    }
}
