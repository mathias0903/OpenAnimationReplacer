#include "OpenAnimationReplacer.h"

#include "Settings.h"
#include "MergeMapperPluginAPI.h"
#include "ReplacementAnimation.h"
#include "ActiveClip.h"
#include "Parsing.h"
#include "DetectedProblems.h"
#include "AnimationFileHashCache.h"
#include "UI/UIManager.h"
#include "Offsets.h"

#include <future>
#include <ranges>

void OpenAnimationReplacer::OnDataLoaded() const
{
    if (Settings::bShowWelcomeBanner) {
        UI::UIManager::GetSingleton().DisplayWelcomeBanner();
    }

    if (Settings::bLoadDefaultBehaviorsInMainMenu && !Settings::bDisablePreloading) {
        InitDefaultProjects();
    }
}

ReplacementAnimation* OpenAnimationReplacer::GetReplacementAnimation(RE::hkbCharacterStringData* a_stringData, RE::hkbClipGenerator* a_clipGenerator, uint16_t a_originalIndex, RE::TESObjectREFR* a_refr) const
{
    if (a_refr) {
        if (const auto projectData = GetReplacerProjectData(a_stringData)) {
            return projectData->EvaluateConditionsAndGetReplacementAnimation(a_clipGenerator, a_originalIndex, a_refr);
        }
    }

    return nullptr;
}

ReplacementAnimation* OpenAnimationReplacer::GetReplacementAnimation(RE::hkbCharacter* a_character, RE::hkbClipGenerator* a_clipGenerator, uint16_t a_originalIndex) const
{
    if (bIsPreLoading) {
        return nullptr;
    }

    if (a_originalIndex != static_cast<uint16_t>(-1)) {
        if (const auto stringData = Utils::GetStringDataFromHkbCharacter(a_character)) {
            const RE::BShkbAnimationGraph* animGraph = SKSE::stl::adjust_pointer<RE::BShkbAnimationGraph>(a_character, -0xC0);
            RE::Actor* actor = animGraph->holder;

            return GetReplacementAnimation(stringData, a_clipGenerator, a_originalIndex, actor);
        }
    }

    return nullptr;
}

bool OpenAnimationReplacer::HasReplacementData(RE::hkbCharacterStringData* a_stringData) const
{
    ReadLocker locker(_dataLock);

    return _replacerProjectDatas.contains(a_stringData);
}

bool OpenAnimationReplacer::RemoveReplacementData(RE::hkbCharacterStringData* a_stringData)
{
    WriteLocker locker(_dataLock);

    return _replacerProjectDatas.erase(a_stringData);
}

ReplacerMod* OpenAnimationReplacer::GetReplacerMod(std::string_view a_path) const
{
    ReadLocker locker(_modLock);

    const auto it = _replacerMods.find(a_path.data());
    if (it != _replacerMods.end()) {
        return it->second.get();
    }

    return nullptr;
}

ReplacerMod* OpenAnimationReplacer::GetReplacerModByName(std::string_view a_name) const
{
	ReadLocker locker(_replacerModNameLock);

    const auto it = _replacerModNameMap.find(a_name.data());
    if (it != _replacerModNameMap.end()) {
        return it->second;
    }

    return nullptr;
}

void OpenAnimationReplacer::AddReplacerMod(std::string_view a_path, std::unique_ptr<ReplacerMod>& a_replacerMod)
{
    auto ptr = a_replacerMod.get();

	{
		WriteLocker locker(_modLock);

		_replacerMods.emplace(a_path, std::move(a_replacerMod));
	}

	{
	    WriteLocker locker(_replacerModNameLock);

		_replacerModNameMap.emplace(ptr->GetName(), ptr);
	}
}

ReplacerMod* OpenAnimationReplacer::GetOrCreateLegacyReplacerMod()
{
    if (!_legacyReplacerMod) {
        _legacyReplacerMod = std::make_unique<ReplacerMod>(""sv, "Legacy"sv, ""sv, "All mods structured in Dynamic Animation Replacer's format."sv, true);
    }

    return _legacyReplacerMod.get();
}

void OpenAnimationReplacer::OnReplacerModNameChanged(std::string_view a_previousName, ReplacerMod* a_replacerMod)
{
    WriteLocker locker(_replacerModNameLock);

    auto handle = _replacerModNameMap.extract(a_previousName.data());
	handle.key() = a_replacerMod->GetName();
	_replacerModNameMap.insert(std::move(handle));
}

void OpenAnimationReplacer::InitializeReplacementAnimations(RE::hkbCharacterStringData* a_stringData) const
{
    if (const auto projectData = GetReplacerProjectData(a_stringData)) {
        projectData->ForEach([](auto a_animationReplacements) {
            a_animationReplacements->TestInterruptible();
            a_animationReplacements->TestKeepRandomResultsOnLoop();
            a_animationReplacements->SortByPriority();
        });
    }
}

AnimationReplacements* OpenAnimationReplacer::GetReplacements(RE::hkbCharacter* a_character, uint16_t a_originalIndex) const
{
    if (a_originalIndex != static_cast<uint16_t>(-1)) {
        if (const auto stringData = Utils::GetStringDataFromHkbCharacter(a_character)) {
            if (const auto replacerProjectData = GetReplacerProjectData(stringData)) {
                return replacerProjectData->GetAnimationReplacements(a_originalIndex);
            }
        }
    }

    return nullptr;
}

ActiveClip* OpenAnimationReplacer::GetActiveClip(RE::hkbClipGenerator* a_clipGenerator) const
{
    ReadLocker locker(_activeClipsLock);

    if (const auto search = _activeClips.find(a_clipGenerator); search != _activeClips.end()) {
        return search->second.get();
    }

    return nullptr;
}

ActiveClip* OpenAnimationReplacer::GetActiveClipFromRefr(RE::TESObjectREFR* a_refr) const
{
    ReadLocker locker(_activeClipsLock);

    if (const auto search = std::ranges::find_if(_activeClips, [&](const auto& pair) {
        return pair.second->GetRefr() == a_refr;
    }); search != _activeClips.end()) {
        return search->second.get();
    }

    return nullptr;
}

ActiveClip* OpenAnimationReplacer::AddOrGetActiveClip(RE::hkbClipGenerator* a_clipGenerator, const RE::hkbContext& a_context, bool& a_bOutAdded)
{
    WriteLocker locker(_activeClipsLock);

    auto [newClipIt, result] = _activeClips.try_emplace(a_clipGenerator, nullptr);
    if (result) {
        newClipIt->second = std::make_unique<ActiveClip>(a_clipGenerator, a_context.character);
    }

    a_bOutAdded = result;
    return newClipIt->second.get();
}

void OpenAnimationReplacer::RemoveActiveClip(RE::hkbClipGenerator* a_clipGenerator)
{
    WriteLocker locker(_activeClipsLock);

    if (const auto search = _activeClips.find(a_clipGenerator); search != _activeClips.end()) {
        if (const auto& activeClip = search->second; !activeClip->IsTransitioning()) {
            _activeClips.erase(search);
        }
    }
}

bool OpenAnimationReplacer::IsOriginalAnimationInterruptible(RE::hkbCharacter* a_character, uint16_t a_originalIndex) const
{
    if (a_originalIndex != static_cast<uint16_t>(-1) && a_character) {
        if (const auto stringData = Utils::GetStringDataFromHkbCharacter(a_character)) {
            if (const auto replacerProjectData = GetReplacerProjectData(stringData)) {
                if (const auto animationReplacements = replacerProjectData->GetAnimationReplacements(a_originalIndex)) {
                    return animationReplacements->IsOriginalInterruptible();
                }
            }
        }
    }

    return false;
}

bool OpenAnimationReplacer::ShouldOriginalAnimationKeepRandomResultsOnLoop(RE::hkbCharacter* a_character, uint16_t a_originalIndex) const
{
    if (a_originalIndex != static_cast<uint16_t>(-1) && a_character) {
        if (const auto stringData = Utils::GetStringDataFromHkbCharacter(a_character)) {
            if (const auto replacerProjectData = GetReplacerProjectData(stringData)) {
                if (const auto animationReplacements = replacerProjectData->GetAnimationReplacements(a_originalIndex)) {
                    return animationReplacements->ShouldOriginalKeepRandomResultsOnLoop();
                }
            }
        }
    }

    return false;
}

template <typename T>
std::future<T> MakeFuture(T& a_t)
{
    std::promise<T> p;
    p.set_value(std::forward<T>(a_t));
    return p.get_future();
}

void OpenAnimationReplacer::CreateReplacementAnimations(const char* a_path, RE::hkbCharacterStringData* a_stringData, RE::BShkbHkxDB::ProjectDBData* a_projectDBData)
{
    if (a_stringData) {
        if (HasReplacementData(a_stringData)) {
            return;
        }

        Locker locker(_parseLock);

        if (!AreFactoriesInitialized()) {
            InitFactories();
        }

        std::string projectPath;
        projectPath = "Data\\Meshes\\"sv;
        projectPath.append(a_path);

        std::string legacyPath = projectPath;
        legacyPath.append("Animations\\DynamicAnimationReplacer\\"sv);

        std::string replacementsPath = projectPath;
        replacementsPath.append("Animations\\OpenAnimationReplacer\\"sv);

        std::vector<std::future<Parsing::ModParseResult>> modFutures;
        std::vector<std::future<Parsing::SubModParseResult>> legacyFutures;

        if (std::filesystem::is_directory(replacementsPath)) {
            // we're in the OAR folder
            if (Settings::bAsyncParsing) {
                for (const auto& entry : std::filesystem::directory_iterator(replacementsPath)) {
                    if (is_directory(entry)) {
                        // we're in a mod folder. we have the subfolders here and a json.
                        modFutures.emplace_back(std::async(std::launch::async, Parsing::ParseModDirectory, entry, a_stringData));
                    }
                }
            } else {
                for (const auto& entry : std::filesystem::directory_iterator(replacementsPath)) {
                    if (is_directory(entry)) {
                        // we're in a mod folder. we have the subfolders here and a json.
                        auto modParseResult = Parsing::ParseModDirectory(entry, a_stringData);
                        modFutures.emplace_back(MakeFuture(modParseResult));
                    }
                }
            }
        }

        if (std::filesystem::is_directory(legacyPath)) {
            // we're in the DAR folder
            for (const auto& entry : std::filesystem::directory_iterator(legacyPath)) {
                if (is_directory(entry)) {
                    std::string stemString;
                    try {
                        stemString = entry.path().stem().string();
                    } catch (const std::system_error&) {
                        auto path = entry.path().u8string();
                        std::string_view pathSv(reinterpret_cast<const char*>(path.data()), path.size());
                        logger::warn("invalid directory name at {}, skipping", pathSv);
                        continue;
                    }
                    if (Utils::CompareStringsIgnoreCase(stemString, "_CustomConditions"sv)) {
                        // we're in the _CustomConditions directory
                        for (const auto& subEntry : std::filesystem::directory_iterator(entry)) {
                            if (is_directory(subEntry)) {
                                legacyFutures.emplace_back(std::async(std::launch::async, Parsing::ParseLegacyCustomConditionsDirectory, subEntry, a_stringData));
                            }
                        }
                    } else {
                        // we're probably in a folder with a plugin name
                        for (auto subModParseResults = Parsing::ParseLegacyPluginDirectory(entry, a_stringData); auto& subModParseResult : subModParseResults) {
                            if (subModParseResult.bSuccess) {
                                legacyFutures.emplace_back(MakeFuture(subModParseResult));
                            }
                        }
                    }
                }
            }
        }

        if (modFutures.empty() && legacyFutures.empty()) {
            return;
        }

        // add all parsed mods
        for (auto& future : modFutures) {
            auto modParseResult = future.get();
            AddModParseResult(modParseResult, a_stringData, a_projectDBData);
        }

        // add all parsed legacy mods
        for (auto& future : legacyFutures) {
            if (auto subModParseResult = future.get(); subModParseResult.bSuccess) {
                auto replacerMod = GetOrCreateLegacyReplacerMod();
                AddSubModParseResult(replacerMod, subModParseResult, a_stringData, a_projectDBData);
            }
        }

        // Save synchronized clip offset
		SetSynchronizedClipsIDOffset(a_stringData, static_cast<uint16_t>(a_stringData->animationNames.size()));

        // If we just added any replacement anims, do stuff
        if (HasReplacementData(a_stringData)) {
            InitializeReplacementAnimations(a_stringData);

            DetectedProblems::GetSingleton().CheckForSubModsSharingPriority();

            if (Settings::bFilterOutDuplicateAnimations) {
                if (auto projectData = GetReplacerProjectData(a_stringData)) {
                    logger::info("Filtered out {} duplicate animations in project {}", projectData->GetFilteredDuplicateCount(), projectData->stringData->name.data());
                }
            }

            auto& animationFileHashCache = AnimationFileHashCache::GetSingleton();
            if (animationFileHashCache.IsDirty()) {
                animationFileHashCache.WriteCacheToDisk();
            }
        }
    }
}

ReplacerProjectData* OpenAnimationReplacer::GetReplacerProjectData(RE::hkbCharacterStringData* a_stringData) const
{
    ReadLocker locker(_dataLock);

    if (const auto search = _replacerProjectDatas.find(a_stringData); search != _replacerProjectDatas.end()) {
        return search->second.get();
    }

    return nullptr;
}

ReplacerProjectData* OpenAnimationReplacer::GetOrAddReplacerProjectData(RE::hkbCharacterStringData* a_stringData, RE::BShkbHkxDB::ProjectDBData* a_projectDBData)
{
    if (const auto replacerProjectData = GetReplacerProjectData(a_stringData)) {
        return replacerProjectData;
    }

    WriteLocker locker(_dataLock);

    auto [it, bSuccess] = _replacerProjectDatas.emplace(a_stringData, std::make_unique<ReplacerProjectData>(a_stringData, a_projectDBData));
    return it->second.get();
}

void OpenAnimationReplacer::ForEachReplacerProjectData(const std::function<void(RE::hkbCharacterStringData*, ReplacerProjectData*)>& a_func) const
{
    ReadLocker locker(_dataLock);

    for (auto& [stringData, replacerProjectData] : _replacerProjectDatas) {
        a_func(stringData, replacerProjectData.get());
    }
}

void OpenAnimationReplacer::ForEachReplacerMod(const std::function<void(ReplacerMod*)>& a_func) const
{
    ReadLocker locker(_modLock);

    for (const auto& replacerMod : _replacerMods | std::views::values) {
        a_func(replacerMod.get());
    }

    if (_legacyReplacerMod) {
        a_func(_legacyReplacerMod.get());
    }
}


void OpenAnimationReplacer::SetSynchronizedClipsIDOffset(RE::hkbCharacterStringData* a_stringData, uint16_t a_offset)
{
    if (const auto search = _replacerProjectDatas.find(a_stringData); search != _replacerProjectDatas.end()) {
        const auto& replacerProjectData = search->second;
        replacerProjectData->synchronizedClipIDOffset = a_offset;
    }
}

uint16_t OpenAnimationReplacer::GetSynchronizedClipsIDOffset(RE::hkbCharacterStringData* a_stringData) const
{
    if (const auto search = _replacerProjectDatas.find(a_stringData); search != _replacerProjectDatas.end()) {
        auto& replacerProjectData = search->second;
        return replacerProjectData->synchronizedClipIDOffset;
    }

    return 0;
}

uint16_t OpenAnimationReplacer::GetSynchronizedClipsIDOffset(RE::hkbCharacter* a_character) const
{
    if (const auto stringData = Utils::GetStringDataFromHkbCharacter(a_character)) {
        return GetSynchronizedClipsIDOffset(stringData);
    }

    return 0;
}

// the loading functions don't actually need a real clip generator, just access two member variables
struct FakeClipGenerator
{
    uint64_t pad00;
    uint64_t pad08;
    uint64_t pad10;
    uint64_t pad18;
    uint64_t pad20;
    uint64_t pad28;
    uint64_t userData = 0;
    uint64_t pad38;
    uint64_t pad40;
    uint64_t pad48;
    uint64_t pad50;
    uint64_t pad58;
    uint64_t pad60;
    uint64_t pad68;
    uint16_t animationBindingIndex = 0;
};

void OpenAnimationReplacer::LoadAnimation(RE::hkbCharacter* a_character, uint16_t a_animationIndex)
{
    FakeClipGenerator fakeClipGenerator{};
    fakeClipGenerator.animationBindingIndex = a_animationIndex;

    const auto clipGenerator = reinterpret_cast<RE::hkbClipGenerator*>(&fakeClipGenerator);

    RE::AnimationFileManagerSingleton::GetSingleton()->Queue(*reinterpret_cast<RE::hkbContext*>(&a_character), clipGenerator, nullptr);
    //RE::AnimationFileManagerSingleton::GetSingleton()->Unk_02(*reinterpret_cast<RE::hkbContext*>(&a_character), clipGenerator, nullptr);
}

void OpenAnimationReplacer::UnloadAnimation(RE::hkbCharacter* a_character, uint16_t a_animationIndex)
{
    FakeClipGenerator fakeClipGenerator{};
    const auto clipGenerator = reinterpret_cast<RE::hkbClipGenerator*>(&fakeClipGenerator);

    fakeClipGenerator.animationBindingIndex = a_animationIndex;

    RE::AnimationFileManagerSingleton::GetSingleton()->Unload(*reinterpret_cast<RE::hkbContext*>(&a_character), clipGenerator, nullptr);
}

void OpenAnimationReplacer::InitFactories()
{
    using namespace Conditions;

    Locker locker(_factoriesLock);

    if (_bFactoriesInitialized) {
        return;
    }

    // Init core condition factories
    _conditionFactories.emplace("IsForm", []() { return std::make_unique<IsFormCondition>(); });
    _conditionFactories.emplace("OR", []() { return std::make_unique<ORCondition>(); });
    _conditionFactories.emplace("AND", []() { return std::make_unique<ANDCondition>(); });
    _conditionFactories.emplace("IsEquipped", []() { return std::make_unique<IsEquippedCondition>(); });
    _conditionFactories.emplace("IsEquippedType", []() { return std::make_unique<IsEquippedTypeCondition>(); });
    _conditionFactories.emplace("IsEquippedHasKeyword", []() { return std::make_unique<IsEquippedHasKeywordCondition>(); });
    _conditionFactories.emplace("IsEquippedPower", []() { return std::make_unique<IsEquippedPowerCondition>(); });
    _conditionFactories.emplace("IsWorn", []() { return std::make_unique<IsWornCondition>(); });
    _conditionFactories.emplace("IsWornHasKeyword", []() { return std::make_unique<IsWornHasKeywordCondition>(); });
    _conditionFactories.emplace("IsFemale", []() { return std::make_unique<IsFemaleCondition>(); });
    _conditionFactories.emplace("IsChild", []() { return std::make_unique<IsChildCondition>(); });
    _conditionFactories.emplace("IsPlayerTeammate", []() { return std::make_unique<IsPlayerTeammateCondition>(); });
    _conditionFactories.emplace("IsInInterior", []() { return std::make_unique<IsInInteriorCondition>(); });
    _conditionFactories.emplace("IsInFaction", []() { return std::make_unique<IsInFactionCondition>(); });
    _conditionFactories.emplace("HasKeyword", []() { return std::make_unique<HasKeywordCondition>(); });
    _conditionFactories.emplace("HasMagicEffect", []() { return std::make_unique<HasMagicEffectCondition>(); });
    _conditionFactories.emplace("HasMagicEffectWithKeyword", []() { return std::make_unique<HasMagicEffectWithKeywordCondition>(); });
    _conditionFactories.emplace("HasPerk", []() { return std::make_unique<HasPerkCondition>(); });
    _conditionFactories.emplace("HasSpell", []() { return std::make_unique<HasSpellCondition>(); });
    _conditionFactories.emplace("CompareValue", []() { return std::make_unique<CompareValue>(); });
    _conditionFactories.emplace("Level", []() { return std::make_unique<LevelCondition>(); });
    _conditionFactories.emplace("IsActorBase", []() { return std::make_unique<IsActorBaseCondition>(); });
    _conditionFactories.emplace("IsRace", []() { return std::make_unique<IsRaceCondition>(); });
    _conditionFactories.emplace("CurrentWeather", []() { return std::make_unique<CurrentWeatherCondition>(); });
    _conditionFactories.emplace("CurrentGameTime", []() { return std::make_unique<CurrentGameTimeCondition>(); });
    _conditionFactories.emplace("Random", []() { return std::make_unique<RandomCondition>(); });
    _conditionFactories.emplace("IsUnique", []() { return std::make_unique<IsUniqueCondition>(); });
    _conditionFactories.emplace("IsClass", []() { return std::make_unique<IsClassCondition>(); });
    _conditionFactories.emplace("IsCombatStyle", []() { return std::make_unique<IsCombatStyleCondition>(); });
    _conditionFactories.emplace("IsVoiceType", []() { return std::make_unique<IsVoiceTypeCondition>(); });
    _conditionFactories.emplace("IsAttacking", []() { return std::make_unique<IsAttackingCondition>(); });
    _conditionFactories.emplace("IsRunning", []() { return std::make_unique<IsRunningCondition>(); });
    _conditionFactories.emplace("IsSneaking", []() { return std::make_unique<IsSneakingCondition>(); });
    _conditionFactories.emplace("IsSprinting", []() { return std::make_unique<IsSprintingCondition>(); });
    _conditionFactories.emplace("IsInAir", []() { return std::make_unique<IsInAirCondition>(); });
    _conditionFactories.emplace("IsInCombat", []() { return std::make_unique<IsInCombatCondition>(); });
    _conditionFactories.emplace("IsWeaponDrawn", []() { return std::make_unique<IsWeaponDrawnCondition>(); });
    _conditionFactories.emplace("IsInLocation", []() { return std::make_unique<IsInLocationCondition>(); });
    _conditionFactories.emplace("HasRefType", []() { return std::make_unique<HasRefTypeCondition>(); });
    _conditionFactories.emplace("IsParentCell", []() { return std::make_unique<IsParentCellCondition>(); });
    _conditionFactories.emplace("IsWorldSpace", []() { return std::make_unique<IsWorldSpaceCondition>(); });
    _conditionFactories.emplace("FactionRank", []() { return std::make_unique<FactionRankCondition>(); });
    _conditionFactories.emplace("IsMovementDirection", []() { return std::make_unique<IsMovementDirectionCondition>(); });
    // ==== END OF LEGACY CONDITIONS ====
    _conditionFactories.emplace("IsEquippedShout", []() { return std::make_unique<IsEquippedShoutCondition>(); });
    _conditionFactories.emplace("HasGraphVariable", []() { return std::make_unique<HasGraphVariableCondition>(); });
    _conditionFactories.emplace("SubmergeLevel", []() { return std::make_unique<SubmergeLevelCondition>(); });
    _conditionFactories.emplace("IsReplacerEnabled", []() { return std::make_unique<IsReplacerEnabledCondition>(); });
	_conditionFactories.emplace("IsCurrentPackage", []() { return std::make_unique<IsCurrentPackageCondition>(); });
	_conditionFactories.emplace("IsWornInSlotHasKeyword", []() { return std::make_unique<IsWornInSlotHasKeywordCondition>(); });
    _conditionFactories.emplace("Scale", []() { return std::make_unique<ScaleCondition>(); });
	_conditionFactories.emplace("Height", []() { return std::make_unique<HeightCondition>(); });
	_conditionFactories.emplace("Weight", []() { return std::make_unique<WeightCondition>(); });
	_conditionFactories.emplace("MovementSpeed", []() { return std::make_unique<MovementSpeedCondition>(); });
	_conditionFactories.emplace("CurrentMovementSpeed", []() { return std::make_unique<CurrentMovementSpeedCondition>(); });
	_conditionFactories.emplace("WindSpeed", []() { return std::make_unique<WindSpeedCondition>(); });
	_conditionFactories.emplace("WindAngleDifference", []() { return std::make_unique<WindAngleDifferenceCondition>(); });
	_conditionFactories.emplace("CrimeGold", []() { return std::make_unique<CrimeGoldCondition>(); });

    // Hidden factories - not visible for selection in the UI, used only for mapping legacy names to new conditions
    _hiddenConditionFactories.emplace("IsEquippedRight", []() { return std::make_unique<IsEquippedCondition>(false); });
    _hiddenConditionFactories.emplace("IsEquippedLeft", []() { return std::make_unique<IsEquippedCondition>(true); });
    _hiddenConditionFactories.emplace("IsEquippedRightType", []() { return std::make_unique<IsEquippedTypeCondition>(false); });
    _hiddenConditionFactories.emplace("IsEquippedLeftType", []() { return std::make_unique<IsEquippedTypeCondition>(true); });
    _hiddenConditionFactories.emplace("IsEquippedRightHasKeyword", []() { return std::make_unique<IsEquippedHasKeywordCondition>(false); });
    _hiddenConditionFactories.emplace("IsEquippedLeftHasKeyword", []() { return std::make_unique<IsEquippedHasKeywordCondition>(true); });
    _hiddenConditionFactories.emplace("ValueEqualTo", []() { return std::make_unique<CompareValue>(ComparisonOperator::kEqual); });
    _hiddenConditionFactories.emplace("ValueLessThan", []() { return std::make_unique<CompareValue>(ComparisonOperator::kLess); });
    _hiddenConditionFactories.emplace("IsActorValueEqualTo", []() { return std::make_unique<CompareValue>(ActorValueType::kActorValue, ComparisonOperator::kEqual); });
    _hiddenConditionFactories.emplace("IsActorValueLessThan", []() { return std::make_unique<CompareValue>(ActorValueType::kActorValue, ComparisonOperator::kLess); });
    _hiddenConditionFactories.emplace("IsActorValueBaseEqualTo", []() { return std::make_unique<CompareValue>(ActorValueType::kBase, ComparisonOperator::kEqual); });
    _hiddenConditionFactories.emplace("IsActorValueBaseLessThan", []() { return std::make_unique<CompareValue>(ActorValueType::kBase, ComparisonOperator::kLess); });
    _hiddenConditionFactories.emplace("IsActorValueMaxEqualTo", []() { return std::make_unique<CompareValue>(ActorValueType::kMax, ComparisonOperator::kEqual); });
    _hiddenConditionFactories.emplace("IsActorValueMaxLessThan", []() { return std::make_unique<CompareValue>(ActorValueType::kMax, ComparisonOperator::kLess); });
    _hiddenConditionFactories.emplace("IsActorValuePercentageEqualTo", []() { return std::make_unique<CompareValue>(ActorValueType::kPercentage, ComparisonOperator::kEqual); });
    _hiddenConditionFactories.emplace("IsActorValuePercentageLessThan", []() { return std::make_unique<CompareValue>(ActorValueType::kPercentage, ComparisonOperator::kLess); });
    _hiddenConditionFactories.emplace("IsFactionRankEqualTo", []() { return std::make_unique<FactionRankCondition>(ComparisonOperator::kEqual); });
    _hiddenConditionFactories.emplace("IsFactionRankLessThan", []() { return std::make_unique<FactionRankCondition>(ComparisonOperator::kLess); });
    _hiddenConditionFactories.emplace("IsLevelLessThan", []() { return std::make_unique<LevelCondition>(ComparisonOperator::kLess); });
    _hiddenConditionFactories.emplace("CurrentGameTimeLessThan", []() { return std::make_unique<CurrentGameTimeCondition>(ComparisonOperator::kLess); });

    for (auto& [name, factory] : _customConditionFactories) {
        _conditionFactories.emplace(name, [&]() { return std::unique_ptr<ICondition>(factory()); });
    }

    _bFactoriesInitialized = true;
}

bool OpenAnimationReplacer::HasConditionFactory(std::string_view a_conditionName) const
{
    return _conditionFactories.contains(a_conditionName.data());
}

void OpenAnimationReplacer::ForEachConditionFactory(const std::function<void(std::string_view, std::function<std::unique_ptr<Conditions::ICondition>()>)>& a_func) const
{
    for (auto& [name, factory] : _conditionFactories) {
        a_func(name, factory);
    }
}

std::unique_ptr<Conditions::ICondition> OpenAnimationReplacer::CreateCondition(std::string_view a_conditionName)
{
    if (const auto search = _conditionFactories.find(a_conditionName.data()); search != _conditionFactories.end()) {
        return search->second();
    }

    if (const auto searchHidden = _hiddenConditionFactories.find(a_conditionName.data()); searchHidden != _hiddenConditionFactories.end()) {
        return searchHidden->second();
    }

    return nullptr;
}

bool OpenAnimationReplacer::IsPluginLoaded(std::string_view a_pluginName, REL::Version a_pluginVersion) const
{
    ReadLocker locker(_customConditionsLock);

    if (const auto search = _customConditionPlugins.find(a_pluginName.data()); search != _customConditionPlugins.end()) {
        return search->second >= a_pluginVersion;
    }

    return false;
}

REL::Version OpenAnimationReplacer::GetPluginVersion(std::string_view a_pluginName) const
{
    ReadLocker locker(_customConditionsLock);

    if (const auto search = _customConditionPlugins.find(a_pluginName.data()); search != _customConditionPlugins.end()) {
        return search->second;
    }

    return 0;
}

OAR_API::Conditions::APIResult OpenAnimationReplacer::AddCustomCondition(std::string_view a_pluginName, REL::Version a_pluginVersion, std::string_view a_conditionName, Conditions::ConditionFactory a_conditionFactory)
{
    using Result = OAR_API::Conditions::APIResult;

    const std::string_view pluginName(a_pluginName);
    const std::string_view conditionName(a_conditionName);

    if (pluginName.empty() || conditionName.empty() || !a_conditionFactory) {
        logger::error("AddCustomCondition - invalid arguments");
        return Result::Invalid;
    }

    if (HasConditionFactory(a_conditionName)) {
        logger::error("AddCustomCondition - condition already exists: {}", a_conditionName);
        return Result::AlreadyRegistered;
    }

	// too late, factories already initialized
	if (_bFactoriesInitialized) {
	    return Result::Failed;
	}

    WriteLocker locker(_customConditionsLock);

    _customConditionPlugins.emplace(a_pluginName, a_pluginVersion);
    _customConditionFactories.emplace(a_conditionName, a_conditionFactory);

    return Result::OK;
}

bool OpenAnimationReplacer::IsCustomCondition(std::string_view a_conditionName) const
{
    ReadLocker locker(_customConditionsLock);

    return _customConditionFactories.contains(a_conditionName.data());
}

void OpenAnimationReplacer::LoadKeywords() const
{
    kywd_weapTypeWarhammer = RE::TESForm::LookupByID<RE::BGSKeyword>(0x6D930);
    kywd_weapTypeBattleaxe = RE::TESForm::LookupByID<RE::BGSKeyword>(0x6D932);
    bKeywordsLoaded = true;
}

void OpenAnimationReplacer::RunJobs()
{
    WriteLocker locker(_jobsLock);

    for (const auto& job : _jobs) {
        job->Run();
    }

    _jobs.clear();
}

void OpenAnimationReplacer::InitDefaultProjects() const
{
    // create a dummy male and female character to force the behaviors to load
    if (const auto npcFactory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESNPC>()) {
        if (auto newNPC = npcFactory->Create()) {
            if (const auto playerBase = RE::TESForm::LookupByID<RE::TESNPC>(0x7)) {
                newNPC->race = playerBase->race;
                TESForm_MakeTemporary(newNPC);

                if (const auto dummyMaleCharacter = CreateDummyCharacter(newNPC)) {
                    dummyMaleCharacter->Load3D(false);
                    //dummyMaleCharacter->Disable();
                    //dummyMaleCharacter->SetDelete(true);
                }

                newNPC->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kFemale);
                if (const auto dummyFemaleCharacter = CreateDummyCharacter(newNPC)) {
                    dummyFemaleCharacter->Load3D(false);
                    //dummyFemaleCharacter->Disable();
                    //dummyFemaleCharacter->SetDelete(true);
                }

                //newNPC->SetDelete(true);
            }
        }
    }
}

RE::Character* OpenAnimationReplacer::CreateDummyCharacter(RE::TESNPC* a_baseForm) const
{
    const size_t size = REL::Module::IsAE() ? 0x2B8 : 0x2B0;
    if (auto dummyCharacter = RE::malloc<RE::Character>(size)) {
        Character_ctor(dummyCharacter);
        TESForm_MakeTemporary(dummyCharacter);
        dummyCharacter->SetObjectReference(a_baseForm);

        return dummyCharacter;
    }

    return nullptr;
}

void OpenAnimationReplacer::AddModParseResult(Parsing::ModParseResult& a_parseResult, RE::hkbCharacterStringData* a_stringData, RE::BShkbHkxDB::ProjectDBData* a_projectDBData)
{
    // Get replacer mod or create it if it doesn't exist
    auto replacerMod = GetReplacerMod(a_parseResult.path);
    if (!replacerMod) {
        auto newReplacerMod = std::make_unique<ReplacerMod>(a_parseResult.path, a_parseResult.name, a_parseResult.author, a_parseResult.description, false);
		replacerMod = newReplacerMod.get();
        AddReplacerMod(a_parseResult.path, newReplacerMod);
		
    }

    for (auto& subModParseResult : a_parseResult.subModParseResults) {
        // Get submod or create it if it doesn't exist
        AddSubModParseResult(replacerMod, subModParseResult, a_stringData, a_projectDBData);
    }
}

void OpenAnimationReplacer::AddSubModParseResult(ReplacerMod* a_replacerMod, Parsing::SubModParseResult& a_parseResult, RE::hkbCharacterStringData* a_stringData, RE::BShkbHkxDB::ProjectDBData* a_projectDBData)
{
    auto subMod = a_replacerMod->GetSubMod(a_parseResult.path);
    if (!subMod) {
        auto newSubMod = std::make_unique<SubMod>();
		newSubMod->LoadParseResult(a_parseResult);
		subMod = newSubMod.get();
		a_replacerMod->AddSubMod(newSubMod);
    }

    // add new replacement anims
    subMod->AddReplacementAnimations(a_stringData, a_projectDBData, a_parseResult.animationsToAdd);
    subMod->AddReplacerProject(GetOrAddReplacerProjectData(a_stringData, a_projectDBData));
}