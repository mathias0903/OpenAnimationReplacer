#pragma once

#include "ReplacerMods.h"

class DetectedProblems
{
public:
    static DetectedProblems& GetSingleton()
    {
        static DetectedProblems singleton;
        return singleton;
    }

    enum class Severity
    {
        kNone,
        kWarning,
        kError
    };

    void MarkOutdatedVersion();
    void AddMissingPluginName(std::string_view a_pluginName, REL::Version a_pluginVersion);
    void CheckForSubModsSharingPriority();

    [[nodiscard]] Severity GetProblemSeverity() const;
    [[nodiscard]] std::string_view GetProblemMessage() const;

    [[nodiscard]] bool IsOutdated() const { return _bIsOutdated; }

    [[nodiscard]] bool HasMissingPlugins() const { return !_missingPlugins.empty(); }
    void ForEachMissingPlugin(const std::function<void(const std::pair<std::string, REL::Version>&)>& a_func) const;
    [[nodiscard]] size_t NumMissingPlugins() const { return _missingPlugins.size(); };

    [[nodiscard]] bool HasSubModsSharingPriority() const { return !_subModsSharingPriority.empty(); }
    void ForEachSubModSharingPriority(const std::function<void(const SubMod*)>& a_func) const;
    [[nodiscard]] size_t NumSubModsSharingPriority() const { return _subModsSharingPriority.size(); };

private:
    DetectedProblems() = default;
    DetectedProblems(const DetectedProblems&) = delete;
    DetectedProblems(DetectedProblems&&) = delete;
    virtual ~DetectedProblems() = default;

    DetectedProblems& operator=(const DetectedProblems&) = delete;
    DetectedProblems& operator=(DetectedProblems&&) = delete;

    bool _bIsOutdated = false;
    std::set<std::pair<std::string, REL::Version>> _missingPlugins;
    std::map<int32_t, std::set<const SubMod*>> _subModsSharingPriority;

    mutable SharedLock _dataLock;
};
