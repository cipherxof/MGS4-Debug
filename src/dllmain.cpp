#include "MinHook.h"
#include "config.h"
#include "debug.h"
#include "utils.h"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

namespace
{
    constexpr char Version[] = "0.1.0";
    constexpr wchar_t ConfigName[] = L"MGS4Debug.ini";
    constexpr wchar_t LogName[] = L"MGS4Debug.log";
    constexpr char LogPattern[] = "[%Y-%m-%d %H:%M:%S.%e] [MGS4Debug] [%l] %v";

    HMODULE PluginModule = nullptr;
    std::shared_ptr<spdlog::logger> Logger;

    bool InitializeLogger(const std::filesystem::path& gameRoot)
    {
        const std::filesystem::path logDirectory = gameRoot / L"logs";
        std::error_code error;
        std::filesystem::create_directories(logDirectory, error);
        if (error)
        {
            MessageBoxW(nullptr, L"Failed to create the MGS4 logs directory", L"MGS4 Debug", MB_ICONERROR);
            return false;
        }

        try
        {
            Logger = spdlog::basic_logger_mt("MGS4Debug", (logDirectory / LogName).string(), true);
            Logger->set_level(spdlog::level::debug);
            Logger->flush_on(spdlog::level::debug);
            spdlog::set_default_logger(Logger);
            spdlog::set_pattern(LogPattern);
            return true;
        }
        catch (const spdlog::spdlog_ex& exception)
        {
            MessageBoxA(nullptr, exception.what(), "MGS4 Debug logging error", MB_ICONERROR);
            return false;
        }
    }

    DWORD WINAPI MainThread(void*)
    {
        HMODULE gameModule = GetModuleHandleW(nullptr);
        const std::filesystem::path gamePath = Utils::GetModulePath(gameModule);
        const std::filesystem::path pluginPath = Utils::GetModulePath(PluginModule);
        if (gamePath.empty() || pluginPath.empty())
            return 1;

        if (!InitializeLogger(gamePath.parent_path()))
            return 1;

        spdlog::info("MGS4 Debug v{} loaded", Version);
        spdlog::info("Plugin: {}", pluginPath.generic_string());

        if (_wcsicmp(gamePath.filename().c_str(), L"mgs4.exe") != 0)
        {
            spdlog::error("Unsupported executable: {}", gamePath.filename().string());
            return 1;
        }

        DebugConfig config;
        if (!LoadConfig(pluginPath.parent_path() / ConfigName, config) || !config.enabled)
        {
            if (!config.enabled)
                spdlog::info("MGS4 Debug is disabled");
            return config.enabled ? 1 : 0;
        }

        Utils::ExecutableSection text;
        if (!Utils::FindModuleSection(gameModule, ".text", text))
        {
            spdlog::error("Failed to locate the MGS4 executable text section");
            return 1;
        }

        const MH_STATUS initializeStatus = MH_Initialize();
        if (initializeStatus != MH_OK && initializeStatus != MH_ERROR_ALREADY_INITIALIZED)
        {
            spdlog::error("Failed to initialize MinHook (status {})", static_cast<int>(initializeStatus));
            return 1;
        }

        if (!MGS4Debug_Install(reinterpret_cast<uintptr_t>(gameModule), text.begin, text.size, config))
        {
            spdlog::error("Failed to install MGS4 Debug");
            MH_Uninitialize();
            return 1;
        }

        const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
        if (enableStatus != MH_OK)
        {
            spdlog::error("Failed to enable MGS4 Debug hooks (status {})", static_cast<int>(enableStatus));
            MH_Uninitialize();
            return 1;
        }

        spdlog::info("MGS4 Debug initialized successfully");
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        PluginModule = module;
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
