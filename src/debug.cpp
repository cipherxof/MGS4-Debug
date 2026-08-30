#include "debug.h"

#include "MinHook.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <winver.h>
#include <windows.h>

namespace
{
    using DebugUiFrameDelegate = void(__fastcall*)();
    using DebugUiRenderDelegate = void(__fastcall*)(uint16_t viewId);
    using DynamicResolutionDebugTabDelegate = void(__fastcall*)(float* dynamicResolution);
    using PerformanceDebugTabDelegate = void(__fastcall*)();
    using MessageDebugTabDelegate = void(__fastcall*)();
    using DofAdjustDebugTabDelegate = void(__fastcall*)();
    using DebugOverlayDelegate = void(__fastcall*)();
    using ImGuiBeginDelegate = bool(__fastcall*)(const char* name, bool* open, uint32_t flags);
    using ImGuiEndDelegate = void(__fastcall*)();
    using ImGuiBeginTabBarDelegate = bool(__fastcall*)(const char* id, uint32_t flags);
    using ImGuiEndTabBarDelegate = void(__fastcall*)();
    using ImGuiBeginTabItemDelegate = bool(__fastcall*)(const char* label, bool* open, uint32_t flags);
    using ImGuiEndTabItemDelegate = void(__fastcall*)();
    using ImGuiCheckboxDelegate = bool(__fastcall*)(const char* label, bool* value);
    using ImGuiItemGetterDelegate = bool(__fastcall*)(void* data, int32_t index, const char** text);
    using ImGuiComboDelegate = bool(__fastcall*)(const char* label, int32_t* currentItem,
        ImGuiItemGetterDelegate getter, void* data, int32_t itemCount, int32_t popupMaxHeight);

    struct ImGuiVec2
    {
        float x;
        float y;
    };

    using ImGuiButtonDelegate = bool(__fastcall*)(const char* label, const ImGuiVec2* size);
    using SetCursorModeDelegate = void(__fastcall*)(int32_t mode, int32_t value);
    using DebugTextDelegate = void(__fastcall*)(uint16_t x, uint16_t y, uint8_t color, const char* text);
    using GetSnakeDelegate = uintptr_t(__fastcall*)();
    using StageRequestBlockedDelegate = int32_t(__fastcall*)();
    using SetStageNameDelegate = void(__fastcall*)(const char* stageName);
    using FinalizeStageRequestDelegate = void(__fastcall*)();

    struct StageMapNode
    {
        StageMapNode* left;
        StageMapNode* parent;
        StageMapNode* right;
        uint8_t color;
        uint8_t isNil;
        uint8_t padding[6];
        char nameStorage[16];
        size_t nameSize;
        size_t nameCapacity;
        uint32_t stageId;
    };

    static_assert(offsetof(StageMapNode, nameStorage) == 0x20);
    static_assert(offsetof(StageMapNode, nameSize) == 0x30);
    static_assert(offsetof(StageMapNode, nameCapacity) == 0x38);
    static_assert(offsetof(StageMapNode, stageId) == 0x40);

    struct StageEntry
    {
        std::string name;
        uint32_t id;
    };

    DebugUiFrameDelegate DebugUiFrame = nullptr;
    DebugUiRenderDelegate DebugUiRender = nullptr;
    DynamicResolutionDebugTabDelegate DynamicResolutionDebugTab = nullptr;
    PerformanceDebugTabDelegate PerformanceDebugTab = nullptr;
    MessageDebugTabDelegate MessageDebugTab = nullptr;
    DofAdjustDebugTabDelegate DofAdjustDebugTab = nullptr;
    DebugOverlayDelegate StageNameDebugOverlay = nullptr;
    DebugOverlayDelegate DifficultyDebugOverlay = nullptr;
    ImGuiBeginDelegate ImGuiBegin = nullptr;
    ImGuiEndDelegate ImGuiEnd = nullptr;
    ImGuiBeginTabBarDelegate ImGuiBeginTabBar = nullptr;
    ImGuiEndTabBarDelegate ImGuiEndTabBar = nullptr;
    ImGuiBeginTabItemDelegate ImGuiBeginTabItem = nullptr;
    ImGuiEndTabItemDelegate ImGuiEndTabItem = nullptr;
    ImGuiCheckboxDelegate ImGuiCheckbox = nullptr;
    ImGuiComboDelegate ImGuiCombo = nullptr;
    ImGuiButtonDelegate ImGuiButton = nullptr;
    SetCursorModeDelegate SetCursorMode = nullptr;
    DebugTextDelegate DebugText = nullptr;
    GetSnakeDelegate GetSnake = nullptr;
    StageRequestBlockedDelegate StageRequestBlocked = nullptr;
    SetStageNameDelegate SetStageName = nullptr;
    FinalizeStageRequestDelegate FinalizeStageRequest = nullptr;

    float* DynamicResolutionState = nullptr;
    uint8_t* DynamicResolutionEnabled = nullptr;
    int32_t* ImGuiRenderGate = nullptr;
    uint8_t* MiscDisplayFlags = nullptr;
    StageMapNode** StageMapHeadStorage = nullptr;
    uint32_t* StageRequestFlags = nullptr;
    uint32_t* FastLoadStageId = nullptr;
    uint32_t* FastLoadMode = nullptr;

    std::atomic_bool MenuVisible{false};
    bool ToggleKeyDown = false;
    bool DisableDynamicResolution = true;
    int ToggleKey = VK_F10;
    std::string GameVersionText;
    std::array<char, 128> SnakeLocationText{};
    ULONGLONG SnakeLocationExpiresAt = 0;
    std::vector<StageEntry> StageEntries;
    int32_t SelectedStage = 0;
    uint32_t PendingStageId = 0;
    std::string PendingStageName;

    constexpr int32_t GlfwCursorMode = 0x33001;
    constexpr int32_t GlfwCursorNormal = 0x34001;
    constexpr int32_t GlfwCursorDisabled = 0x34003;
    constexpr ptrdiff_t DynamicResolutionEnabledOffset = 0x1f8;

    bool IsReadableMemory(const void* address, size_t length)
    {
        if (!address || length == 0)
            return false;

        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }

        const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
        const uintptr_t end = begin + length;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        return end >= begin && end <= regionEnd;
    }

    bool ReadStageName(const StageMapNode* node, std::string& name)
    {
        if (node->nameSize == 0 || node->nameSize >= 16)
            return false;

        const char* source = node->nameStorage;
        if (node->nameCapacity >= sizeof(node->nameStorage))
            std::memcpy(&source, node->nameStorage, sizeof(source));

        if (!IsReadableMemory(source, node->nameSize + 1) || source[node->nameSize] != '\0')
            return false;

        name.assign(source, node->nameSize);
        return true;
    }

    bool CollectStageEntries(StageMapNode* node, StageMapNode* head,
        size_t& visited, std::vector<StageEntry>& entries)
    {
        if (!node || node == head)
            return true;
        if (!IsReadableMemory(node, sizeof(*node)))
            return false;
        if (node->isNil != 0)
            return true;
        if (++visited > 2048)
            return false;

        if (!CollectStageEntries(node->left, head, visited, entries))
            return false;

        std::string name;
        if (node->stageId != 0 && ReadStageName(node, name) && name != "select")
            entries.push_back({std::move(name), node->stageId});

        return CollectStageEntries(node->right, head, visited, entries);
    }

    bool RefreshStageEntries()
    {
        if (!StageMapHeadStorage)
            return false;

        StageMapNode* head = *StageMapHeadStorage;
        if (!IsReadableMemory(head, sizeof(*head)) || head->isNil == 0)
            return false;

        std::vector<StageEntry> entries;

        size_t visited = 0;
        if (!CollectStageEntries(head->parent, head, visited, entries) || entries.empty())
            return false;

        StageEntries = std::move(entries);
        SelectedStage = std::clamp(SelectedStage, 0, static_cast<int32_t>(StageEntries.size() - 1));
        spdlog::info("Loaded {} entries from the MGS4 stage registry", StageEntries.size());
        return true;
    }

    bool __fastcall GetStageItem(void* data, int32_t index, const char** text)
    {
        const auto* entries = static_cast<const std::vector<StageEntry>*>(data);
        if (!entries || !text || index < 0 || static_cast<size_t>(index) >= entries->size())
            return false;

        *text = (*entries)[index].name.c_str();
        return true;
    }

    bool RequestStage(const StageEntry& stage)
    {
        if (StageRequestBlocked() != 0)
        {
            spdlog::warn("Cannot load stage '{}': a stage transition is already active", stage.name);
            return false;
        }

        *FastLoadStageId = stage.id;
        *FastLoadMode = 1;
        SetStageName("select");
        *StageRequestFlags |= 0x11;
        FinalizeStageRequest();
        return true;
    }

    void SetMenuVisible(bool visible)
    {
        MenuVisible.store(visible, std::memory_order_relaxed);
        SetCursorMode(GlfwCursorMode, visible ? GlfwCursorNormal : GlfwCursorDisabled);
        spdlog::info("MGS4 developer UI {}", visible ? "shown" : "hidden");
    }

    std::string GetGameVersionText()
    {
        std::wstring executablePath(32768, L'\0');
        const DWORD pathLength = GetModuleFileNameW(
            nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
        if (pathLength == 0 || pathLength >= executablePath.size())
            return "MGS4 version unavailable";
        executablePath.resize(pathLength);

        DWORD ignored = 0;
        const DWORD versionDataSize = GetFileVersionInfoSizeW(executablePath.c_str(), &ignored);
        if (versionDataSize == 0)
            return "MGS4 version unavailable";

        std::vector<uint8_t> versionData(versionDataSize);
        if (!GetFileVersionInfoW(executablePath.c_str(), 0, versionDataSize, versionData.data()))
            return "MGS4 version unavailable";

        VS_FIXEDFILEINFO* version = nullptr;
        UINT versionSize = 0;
        if (!VerQueryValueW(versionData.data(), L"\\", reinterpret_cast<void**>(&version), &versionSize) ||
            !version || versionSize < sizeof(VS_FIXEDFILEINFO) || version->dwSignature != 0xfeef04bd)
        {
            return "MGS4 version unavailable";
        }

        std::array<char, 64> text{};
        std::snprintf(text.data(), text.size(), "MGS4 %u.%u.%u.%u",
            HIWORD(version->dwFileVersionMS), LOWORD(version->dwFileVersionMS),
            HIWORD(version->dwFileVersionLS), LOWORD(version->dwFileVersionLS));
        return text.data();
    }

    void CaptureSnakeLocation()
    {
        const uintptr_t snakeSlot = GetSnake();
        const auto* snake = snakeSlot != 0
            ? *reinterpret_cast<uint8_t* const*>(snakeSlot + 8)
            : nullptr;
        if (!snake)
        {
            std::snprintf(SnakeLocationText.data(), SnakeLocationText.size(), "Snake is unavailable");
            spdlog::warn("Could not read Snake's location");
        }
        else
        {
            float position[3]{};
            int16_t area = 0;
            std::memcpy(position, snake + 0x10, sizeof(position));
            std::memcpy(&area, snake + 0x52, sizeof(area));
            std::snprintf(SnakeLocationText.data(), SnakeLocationText.size(),
                "Snake: %.3f %.3f %.3f (area %d)",
                position[0], position[1], position[2], static_cast<int>(area));
            spdlog::info("{}", SnakeLocationText.data());
        }
        SnakeLocationExpiresAt = GetTickCount64() + 5000;
    }

    void MiscDeveloperTab()
    {
        if (!ImGuiBeginTabItem("Misc.", nullptr, 0))
            return;

        ImGuiCheckbox("Display Version", reinterpret_cast<bool*>(MiscDisplayFlags + 0));
        ImGuiCheckbox("Display StageName", reinterpret_cast<bool*>(MiscDisplayFlags + 1));
        ImGuiCheckbox("Display Difficulty", reinterpret_cast<bool*>(MiscDisplayFlags + 2));
        ImGuiCheckbox("Display Lockit GlobalId", reinterpret_cast<bool*>(MiscDisplayFlags + 3));

        constexpr ImGuiVec2 ButtonSize{};
        if (ImGuiButton("Output Location of Snake", &ButtonSize))
            CaptureSnakeLocation();

        ImGuiEndTabItem();
    }

    bool StageDeveloperTab()
    {
        if (!ImGuiBeginTabItem("Stage", nullptr, 0))
            return false;

        if (StageEntries.empty())
            RefreshStageEntries();

        if (StageEntries.empty())
        {
            ImGuiEndTabItem();
            return false;
        }

        ImGuiCombo("Stage", &SelectedStage, GetStageItem, &StageEntries,
            static_cast<int32_t>(StageEntries.size()), 18);

        constexpr ImGuiVec2 ButtonSize{};
        const bool loadStage = ImGuiButton("Load Stage", &ButtonSize);
        const StageEntry selected = StageEntries[SelectedStage];
        ImGuiEndTabItem();

        if (!loadStage)
            return false;

        PendingStageId = selected.id;
        PendingStageName = selected.name;
        return true;
    }

    void RenderDeveloperOverlays()
    {
        StageNameDebugOverlay();
        DifficultyDebugOverlay();

        if (MiscDisplayFlags[0] != 0)
            DebugText(0, 2, 0x0f, GameVersionText.c_str());

        if (SnakeLocationText[0] != '\0' && GetTickCount64() < SnakeLocationExpiresAt)
            DebugText(0, 3, 0x0f, SnakeLocationText.data());
    }

    void __fastcall DebugUiFrameHook()
    {
        const bool keyDown = (GetAsyncKeyState(ToggleKey) & 0x8000) != 0;
        if (keyDown && !ToggleKeyDown)
            SetMenuVisible(!MenuVisible.load(std::memory_order_relaxed));
        ToggleKeyDown = keyDown;

        if (DisableDynamicResolution)
            *DynamicResolutionEnabled = 0;

        RenderDeveloperOverlays();
        DebugUiFrame();

        if (!MenuVisible.load(std::memory_order_relaxed))
            return;

        if (ImGuiBegin("MGS4 Developer", nullptr, 0))
        {
            if (ImGuiBeginTabBar("MGS4DeveloperTabs", 0))
            {
                if (!StageDeveloperTab())
                {
                    PerformanceDebugTab();
                    if (*DynamicResolutionEnabled != 0)
                        DynamicResolutionDebugTab(DynamicResolutionState);
                    DofAdjustDebugTab();
                    MiscDeveloperTab();
                    MessageDebugTab();
                }
                ImGuiEndTabBar();
            }
        }
        ImGuiEnd();

        if (PendingStageId != 0)
        {
            const uint32_t stageId = PendingStageId;
            const std::string stageName = std::move(PendingStageName);
            PendingStageId = 0;
            PendingStageName.clear();

            const StageEntry stage{stageName, stageId};
            if (!RequestStage(stage))
                return;

            spdlog::info("Queued fast-load stage '{}' (registry id {:#x}) through the select handoff",
                stageName, stageId);
            SetMenuVisible(false);
        }
    }

    void __fastcall DebugUiRenderHook(uint16_t viewId)
    {
        const bool forceRender = MenuVisible.load(std::memory_order_relaxed);
        const int32_t originalGate = *ImGuiRenderGate;
        if (forceRender)
            *ImGuiRenderGate = 1;

        DebugUiRender(viewId);

        if (forceRender)
            *ImGuiRenderGate = originalGate;
    }

    int64_t __fastcall ZeroReturnHook()
    {
        return 0;
    }

    bool InstallDisableFilter(uintptr_t moduleBase, uint8_t* textBegin, uintptr_t textSize)
    {
        constexpr char Pattern[] =
            "40 53 48 83 EC 20 48 BA 00 00 00 00 ?? ?? ?? ?? B9 60 04 00 00 "
            "E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 4C 8D 0D ?? ?? ?? ?? "
            "48 89 7C 24 30";

        uint8_t* target = Utils::PatternScanRange(textBegin, textSize, Pattern);
        if (!target)
            return false;

        Utils::LogAddress("FUN_7ff720b32a60", reinterpret_cast<uintptr_t>(target), moduleBase);
        return MH_CreateHook(target, reinterpret_cast<LPVOID>(&ZeroReturnHook), nullptr) == MH_OK;
    }

    bool InstallDisableMotionBlur(uintptr_t moduleBase, uint8_t* textBegin, uintptr_t textSize)
    {
        constexpr char Pattern[] =
            "40 53 48 83 EC 70 0F 29 74 24 60 BA FA 00 00 00 B9 12 BF 32 00 "
            "0F 29 7C 24 50 E8 ?? ?? ?? ?? F3 0F 10 3D ?? ?? ?? ?? BA 0A 00 00 00 "
            "B9 73 97 5A 00";

        uint8_t* target = Utils::PatternScanRange(textBegin, textSize, Pattern);
        if (!target)
            return false;

        Utils::LogAddress("FUN_7ff720b19ab0", reinterpret_cast<uintptr_t>(target), moduleBase);
        return MH_CreateHook(target, reinterpret_cast<LPVOID>(&ZeroReturnHook), nullptr) == MH_OK;
    }

    bool InstallDeveloperUi(uintptr_t moduleBase, uint8_t* textBegin, uintptr_t textSize)
    {
        constexpr char UiFramePattern[] =
            "48 83 EC 28 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F B7 90 BA 00 00 00 "
            "0F B7 88 B8 00 00 00 E8 ?? ?? ?? ?? 48 83 C4 28 E9 ?? ?? ?? ??";
        constexpr char UiRenderPattern[] =
            "40 53 48 83 EC 20 0F B7 D9 E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 74 ?? "
            "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D0 48 8D 0D ?? ?? ?? ?? 44 0F B7 C3";
        constexpr char DynamicResolutionTabPattern[] =
            "40 56 48 81 EC D0 00 00 00 48 8B F1 45 33 C0 48 8D 0D ?? ?? ?? ?? 33 D2 "
            "E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 89 AC 24 F0 00 00 00";
        constexpr char PerformanceTabPattern[] =
            "48 8B C4 57 48 81 EC 60 02 00 00 0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8 "
            "44 0F 29 48 98 44 0F 29 50 88 E8 ?? ?? ?? ??";
        constexpr char MessageTabPattern[] =
            "48 83 EC 38 45 33 C0 48 8D 0D ?? ?? ?? ?? 33 D2 E8 ?? ?? ?? ?? 84 C0 "
            "0F 84 ?? ?? ?? ?? 48 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 48 83 3D "
            "?? ?? ?? ?? 00 74 ?? 48 83 3D ?? ?? ?? ?? 10";
        constexpr char DofAdjustTabPattern[] =
            "48 83 EC 38 45 33 C0 48 8D 0D ?? ?? ?? ?? 33 D2 E8 ?? ?? ?? ?? 84 C0 "
            "0F 84 ?? ?? ?? ?? 48 89 7C 24 30 E8 ?? ?? ?? ?? D1 E8 48 8D 54 24 40 24 01";
        constexpr char MiscTabPattern[] =
            "48 81 EC C8 00 00 00 45 33 C0 48 8D 0D ?? ?? ?? ?? 33 D2 E8 ?? ?? ?? ?? "
            "84 C0 0F 84 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";
        constexpr char StageNameOverlayPattern[] =
            "48 83 EC 28 80 3D ?? ?? ?? ?? 00 74 ?? 48 89 5C 24 30 48 C7 C3 FF FF FF FF "
            "48 89 74 24 38 48 8B 35 ?? ?? ?? ??";
        constexpr char DifficultyOverlayPattern[] =
            "48 83 EC 28 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
            "4C 8D 05 ?? ?? ?? ?? 48 89 5C 24 30 48 89 74 24 38 33 F6";
        constexpr char DynamicResolutionInitializerPattern[] =
            "48 81 EC 28 02 00 00 45 8B C8 44 8B C2 8B D1 48 8D 4C 24 20 E8 ?? ?? ?? ?? "
            "48 8D 0D ?? ?? ?? ?? BA 03 00 00 00";
        constexpr char ImGuiBeginPattern[] =
            "48 8B C4 48 89 50 10 48 89 48 08 55 56 57 41 54 41 55 48 8D A8 F8 FE FF FF "
            "48 81 EC E0 01 00 00 4C 8B 2D ?? ?? ?? ?? 48 8B F1 44 0F 29 B0 28 FF FF FF";
        constexpr char ImGuiEndPattern[] =
            "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 8B FB "
            "83 BB 78 11 00 00 01 48 8B B3 A8 11 00 00";
        constexpr char ImGuiBeginTabBarPattern[] =
            "48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B 35 ?? ?? ?? ?? 8B EA "
            "48 8B BE A8 11 00 00 80 BF B3 00 00 00 00 74 12 32 C0";
        constexpr char ImGuiEndTabBarPattern[] =
            "48 89 74 24 10 57 48 83 EC 20 48 8B 3D ?? ?? ?? ?? 48 8B B7 A8 11 00 00 "
            "80 BE B3 00 00 00 00 0F 85 ?? ?? ?? ?? 48 89 5C 24 30";
        constexpr char ImGuiComboPattern[] =
            "48 89 5C 24 18 48 89 6C 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 40 "
            "48 8B 05 ?? ?? ?? ?? 33 FF 44 8B B4 24 90 00 00 00 4C 8B FA 8B 12 4D 8B E1";
        constexpr char CursorModePattern[] =
            "48 89 5C 24 08 57 48 83 EC 20 8B F9 8B DA 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
            "48 8B C8 44 8B C3 8B D7 48 8B 5C 24 30 48 83 C4 20 5F E9 ?? ?? ?? ??";
        constexpr char FastLoadStagePattern[] =
            "40 57 41 56 41 57 48 83 EC 20 48 8B F9 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? "
            "FF 15 ?? ?? ?? ?? 4C 8B 7F 18 48 8B C7 4C 8B 77 10 49 83 FF 10 72 ?? "
            "48 8B 07 49 83 FE 06 75 ?? 81 38 73 65 6C 65 75 ??";
        constexpr char StageRequestHandlerPattern[] =
            "48 83 EC 28 E8 ?? ?? ?? ?? 85 C0 0F 85 ?? ?? ?? ?? 48 89 5C 24 20 "
            "E8 ?? ?? ?? ?? 48 8B D8 80 38 00 0F 84 ?? ?? ?? ?? B1 6E E8 ?? ?? ?? ?? "
            "48 85 C0 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? E8 ?? ?? ?? ?? 85 C0 75 ??";

        uint8_t* uiFrame = Utils::PatternScanRange(textBegin, textSize, UiFramePattern);
        uint8_t* uiRender = Utils::PatternScanRange(textBegin, textSize, UiRenderPattern);
        uint8_t* dynamicResolutionTab = Utils::PatternScanRange(textBegin, textSize, DynamicResolutionTabPattern);
        uint8_t* performanceTab = Utils::PatternScanRange(textBegin, textSize, PerformanceTabPattern);
        uint8_t* messageTab = Utils::PatternScanRange(textBegin, textSize, MessageTabPattern);
        uint8_t* dofAdjustTab = Utils::PatternScanRange(textBegin, textSize, DofAdjustTabPattern);
        uint8_t* miscTab = Utils::PatternScanRange(textBegin, textSize, MiscTabPattern);
        uint8_t* stageNameOverlay = Utils::PatternScanRange(textBegin, textSize, StageNameOverlayPattern);
        uint8_t* difficultyOverlay = Utils::PatternScanRange(textBegin, textSize, DifficultyOverlayPattern);
        uint8_t* dynamicResolutionInitializer = Utils::PatternScanRange(
            textBegin, textSize, DynamicResolutionInitializerPattern);
        uint8_t* imguiBegin = Utils::PatternScanRange(textBegin, textSize, ImGuiBeginPattern);
        uint8_t* imguiEnd = Utils::PatternScanRange(textBegin, textSize, ImGuiEndPattern);
        uint8_t* imguiBeginTabBar = Utils::PatternScanRange(textBegin, textSize, ImGuiBeginTabBarPattern);
        uint8_t* imguiEndTabBar = Utils::PatternScanRange(textBegin, textSize, ImGuiEndTabBarPattern);
        uint8_t* imguiCombo = Utils::PatternScanRange(textBegin, textSize, ImGuiComboPattern);
        uint8_t* cursorMode = Utils::PatternScanRange(textBegin, textSize, CursorModePattern);
        uint8_t* fastLoadStage = Utils::PatternScanRange(textBegin, textSize, FastLoadStagePattern);
        uint8_t* stageRequestHandler = Utils::PatternScanRange(
            textBegin, textSize, StageRequestHandlerPattern);

        if (!uiFrame || !uiRender || !dynamicResolutionTab || !performanceTab || !messageTab ||
            !dofAdjustTab || !miscTab || !stageNameOverlay || !difficultyOverlay ||
            !dynamicResolutionInitializer || !imguiBegin || !imguiEnd || !imguiBeginTabBar ||
            !imguiEndTabBar || !imguiCombo || !cursorMode || !fastLoadStage || !stageRequestHandler)
        {
            spdlog::error("Failed to resolve one or more MGS4 developer UI targets");
            return false;
        }

        constexpr std::array<uint8_t, 2> RenderGateLoadOpcode = {0x83, 0x3d};
        constexpr std::array<uint8_t, 3> DynamicResolutionStateLoadOpcode = {0x48, 0x8d, 0x0d};
        constexpr std::array<uint8_t, 3> MiscFlagsLoadOpcode = {0x48, 0x8d, 0x15};
        constexpr std::array<uint8_t, 3> StageMapHeadLoadOpcode = {0x4c, 0x8b, 0x2d};
        constexpr std::array<uint8_t, 2> FastLoadStageIdStoreOpcode = {0x89, 0x05};
        if (std::memcmp(uiRender + 0x0e, RenderGateLoadOpcode.data(), RenderGateLoadOpcode.size()) != 0 ||
            std::memcmp(dynamicResolutionInitializer + 0x19, DynamicResolutionStateLoadOpcode.data(),
                DynamicResolutionStateLoadOpcode.size()) != 0 ||
            std::memcmp(miscTab + 0x20, MiscFlagsLoadOpcode.data(), MiscFlagsLoadOpcode.size()) != 0 ||
            std::memcmp(fastLoadStage + 0x85, StageMapHeadLoadOpcode.data(),
                StageMapHeadLoadOpcode.size()) != 0 ||
            std::memcmp(fastLoadStage + 0x122, FastLoadStageIdStoreOpcode.data(),
                FastLoadStageIdStoreOpcode.size()) != 0 ||
            stageRequestHandler[0x04] != 0xe8 || stageRequestHandler[0xa8] != 0xe8 ||
            stageRequestHandler[0xad] != 0x83 || stageRequestHandler[0xae] != 0x0d ||
            stageRequestHandler[0xb3] != 0x01 || stageRequestHandler[0xd2] != 0xe8 ||
            miscTab[0x13] != 0xe8 || miscTab[0x2e] != 0xe8 || miscTab[0x87] != 0xe8 ||
            miscTab[0x98] != 0xe8 || miscTab[0x102] != 0xe8 || stageNameOverlay[0x77] != 0xe8)
        {
            spdlog::error("MGS4 developer UI data-reference validation failed");
            return false;
        }

        int32_t renderGateDisplacement = 0;
        std::memcpy(&renderGateDisplacement, uiRender + 0x10, sizeof(renderGateDisplacement));
        ImGuiRenderGate = reinterpret_cast<int32_t*>(uiRender + 0x15 + renderGateDisplacement);
        DynamicResolutionState = reinterpret_cast<float*>(
            Utils::ResolveRelativeAddress(dynamicResolutionInitializer + 0x1c));
        DynamicResolutionEnabled = reinterpret_cast<uint8_t*>(DynamicResolutionState) +
            DynamicResolutionEnabledOffset;
        MiscDisplayFlags = reinterpret_cast<uint8_t*>(Utils::ResolveRelativeAddress(miscTab + 0x23));
        StageMapHeadStorage = reinterpret_cast<StageMapNode**>(
            Utils::ResolveRelativeAddress(fastLoadStage + 0x88));
        FastLoadStageId = reinterpret_cast<uint32_t*>(
            Utils::ResolveRelativeAddress(fastLoadStage + 0x124));
        FastLoadMode = FastLoadStageId + 1;
        StageRequestFlags = reinterpret_cast<uint32_t*>(
            Utils::ResolveRelativeAddress(stageRequestHandler + 0xaf) + 1);

        DynamicResolutionDebugTab = reinterpret_cast<DynamicResolutionDebugTabDelegate>(dynamicResolutionTab);
        PerformanceDebugTab = reinterpret_cast<PerformanceDebugTabDelegate>(performanceTab);
        MessageDebugTab = reinterpret_cast<MessageDebugTabDelegate>(messageTab);
        DofAdjustDebugTab = reinterpret_cast<DofAdjustDebugTabDelegate>(dofAdjustTab);
        StageNameDebugOverlay = reinterpret_cast<DebugOverlayDelegate>(stageNameOverlay);
        DifficultyDebugOverlay = reinterpret_cast<DebugOverlayDelegate>(difficultyOverlay);
        ImGuiBeginTabItem = reinterpret_cast<ImGuiBeginTabItemDelegate>(
            Utils::ResolveRelativeAddress(miscTab + 0x14));
        ImGuiCheckbox = reinterpret_cast<ImGuiCheckboxDelegate>(
            Utils::ResolveRelativeAddress(miscTab + 0x2f));
        ImGuiButton = reinterpret_cast<ImGuiButtonDelegate>(
            Utils::ResolveRelativeAddress(miscTab + 0x88));
        GetSnake = reinterpret_cast<GetSnakeDelegate>(Utils::ResolveRelativeAddress(miscTab + 0x99));
        ImGuiEndTabItem = reinterpret_cast<ImGuiEndTabItemDelegate>(
            Utils::ResolveRelativeAddress(miscTab + 0x103));
        DebugText = reinterpret_cast<DebugTextDelegate>(
            Utils::ResolveRelativeAddress(stageNameOverlay + 0x78));
        ImGuiBegin = reinterpret_cast<ImGuiBeginDelegate>(imguiBegin);
        ImGuiEnd = reinterpret_cast<ImGuiEndDelegate>(imguiEnd);
        ImGuiBeginTabBar = reinterpret_cast<ImGuiBeginTabBarDelegate>(imguiBeginTabBar);
        ImGuiEndTabBar = reinterpret_cast<ImGuiEndTabBarDelegate>(imguiEndTabBar);
        ImGuiCombo = reinterpret_cast<ImGuiComboDelegate>(imguiCombo);
        SetCursorMode = reinterpret_cast<SetCursorModeDelegate>(cursorMode);
        StageRequestBlocked = reinterpret_cast<StageRequestBlockedDelegate>(
            Utils::ResolveRelativeAddress(stageRequestHandler + 0x05));
        SetStageName = reinterpret_cast<SetStageNameDelegate>(
            Utils::ResolveRelativeAddress(stageRequestHandler + 0xa9));
        FinalizeStageRequest = reinterpret_cast<FinalizeStageRequestDelegate>(
            Utils::ResolveRelativeAddress(stageRequestHandler + 0xd3));
        GameVersionText = GetGameVersionText();

        Utils::LogAddress("debugUiFrame", reinterpret_cast<uintptr_t>(uiFrame), moduleBase);
        Utils::LogAddress("debugUiRender", reinterpret_cast<uintptr_t>(uiRender), moduleBase);
        Utils::LogAddress("dynamicResolutionDebugTab", reinterpret_cast<uintptr_t>(dynamicResolutionTab), moduleBase);
        Utils::LogAddress("performanceDebugTab", reinterpret_cast<uintptr_t>(performanceTab), moduleBase);
        Utils::LogAddress("messageDebugTab", reinterpret_cast<uintptr_t>(messageTab), moduleBase);
        Utils::LogAddress("dofAdjustDebugTab", reinterpret_cast<uintptr_t>(dofAdjustTab), moduleBase);
        Utils::LogAddress("miscDebugTab", reinterpret_cast<uintptr_t>(miscTab), moduleBase);
        Utils::LogAddress("stageNameDebugOverlay", reinterpret_cast<uintptr_t>(stageNameOverlay), moduleBase);
        Utils::LogAddress("difficultyDebugOverlay", reinterpret_cast<uintptr_t>(difficultyOverlay), moduleBase);
        Utils::LogAddress("debugText", reinterpret_cast<uintptr_t>(DebugText), moduleBase);
        Utils::LogAddress("getSnake", reinterpret_cast<uintptr_t>(GetSnake), moduleBase);
        Utils::LogAddress("imguiBegin", reinterpret_cast<uintptr_t>(imguiBegin), moduleBase);
        Utils::LogAddress("imguiEnd", reinterpret_cast<uintptr_t>(imguiEnd), moduleBase);
        Utils::LogAddress("imguiBeginTabBar", reinterpret_cast<uintptr_t>(imguiBeginTabBar), moduleBase);
        Utils::LogAddress("imguiEndTabBar", reinterpret_cast<uintptr_t>(imguiEndTabBar), moduleBase);
        Utils::LogAddress("imguiCombo", reinterpret_cast<uintptr_t>(imguiCombo), moduleBase);
        Utils::LogAddress("dynamicResolutionState", reinterpret_cast<uintptr_t>(DynamicResolutionState), moduleBase);
        Utils::LogAddress("imguiRenderGate", reinterpret_cast<uintptr_t>(ImGuiRenderGate), moduleBase);
        Utils::LogAddress("setCursorMode", reinterpret_cast<uintptr_t>(cursorMode), moduleBase);
        Utils::LogAddress("fastLoadStage", reinterpret_cast<uintptr_t>(fastLoadStage), moduleBase);
        Utils::LogAddress("stageMapHead", reinterpret_cast<uintptr_t>(StageMapHeadStorage), moduleBase);
        Utils::LogAddress("fastLoadStageId", reinterpret_cast<uintptr_t>(FastLoadStageId), moduleBase);
        Utils::LogAddress("fastLoadMode", reinterpret_cast<uintptr_t>(FastLoadMode), moduleBase);
        Utils::LogAddress("stageRequestFlags", reinterpret_cast<uintptr_t>(StageRequestFlags), moduleBase);
        Utils::LogAddress("stageRequestBlocked", reinterpret_cast<uintptr_t>(StageRequestBlocked), moduleBase);
        Utils::LogAddress("setStageName", reinterpret_cast<uintptr_t>(SetStageName), moduleBase);
        Utils::LogAddress("finalizeStageRequest", reinterpret_cast<uintptr_t>(FinalizeStageRequest), moduleBase);
        spdlog::info("Game executable version text: {}", GameVersionText);

        if (MH_CreateHook(uiFrame, reinterpret_cast<LPVOID>(&DebugUiFrameHook),
                reinterpret_cast<void**>(&DebugUiFrame)) != MH_OK)
        {
            return false;
        }

        if (MH_CreateHook(uiRender, reinterpret_cast<LPVOID>(&DebugUiRenderHook),
                reinterpret_cast<void**>(&DebugUiRender)) != MH_OK)
        {
            MH_RemoveHook(uiFrame);
            DebugUiFrame = nullptr;
            return false;
        }

        spdlog::info("MGS4 developer UI installed; press virtual key {:#x} to toggle it", ToggleKey);
        return true;
    }
}

bool MGS4Debug_Install(uintptr_t moduleBase, uint8_t* textBegin, uintptr_t textSize,
    const DebugConfig& config)
{
    ToggleKey = config.toggleKey;
    DisableDynamicResolution = config.disableDynamicResolution;

    if (!InstallDeveloperUi(moduleBase, textBegin, textSize))
        return false;

    if (DisableDynamicResolution)
    {
        *DynamicResolutionEnabled = 0;
        spdlog::info("Dynamic resolution disabled by config");
    }

    if (config.disableFilter && !InstallDisableFilter(moduleBase, textBegin, textSize))
    {
        spdlog::error("Failed to disable the filter (FUN_7ff720b32a60)");
        return false;
    }

    if (config.disableMotionBlur && !InstallDisableMotionBlur(moduleBase, textBegin, textSize))
    {
        spdlog::error("Failed to disable motion blur (FUN_7ff720b19ab0)");
        return false;
    }

    return true;
}
