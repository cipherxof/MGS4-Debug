#include "config.h"

#include "utils.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <spdlog/spdlog.h>

namespace
{
    std::wstring ReadIniValue(const std::filesystem::path& path, const wchar_t* key, const wchar_t* fallback)
    {
        std::array<wchar_t, 256> value{};
        GetPrivateProfileStringW(
            L"Debug", key, fallback, value.data(), static_cast<DWORD>(value.size()), path.c_str());
        return value.data();
    }

    int ParseVirtualKey(std::wstring value, int fallback)
    {
        value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t character) {
            return std::iswspace(character) != 0;
        }), value.end());
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towupper(character));
        });

        if (value.size() >= 2 && value[0] == L'F')
        {
            wchar_t* end = nullptr;
            const long number = std::wcstol(value.c_str() + 1, &end, 10);
            if (end && *end == L'\0' && number >= 1 && number <= 24)
                return VK_F1 + static_cast<int>(number) - 1;
        }

        if (value.size() == 1 && ((value[0] >= L'A' && value[0] <= L'Z') ||
                                 (value[0] >= L'0' && value[0] <= L'9')))
        {
            return static_cast<int>(value[0]);
        }

        if (!value.empty())
        {
            wchar_t* end = nullptr;
            const long number = std::wcstol(value.c_str(), &end, 0);
            if (end && *end == L'\0' && number > 0 && number <= 0xff)
                return static_cast<int>(number);
        }

        return fallback;
    }
}

bool LoadConfig(const std::filesystem::path& path, DebugConfig& config)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        std::ofstream file(path);
        if (!file)
        {
            spdlog::error("Failed to create config file {}", path.generic_string());
            return false;
        }
        file << "[Debug]\n"
                "Enabled = true\n"
                "ToggleKey = F10\n"
                "DisableFilter = true\n"
                "DisableMotionBlur = true\n";
        spdlog::info("Created config file {}", path.generic_string());
    }

    config.enabled = Utils::ParseBoolean(ReadIniValue(path, L"Enabled", L"true"), true);
    config.toggleKey = ParseVirtualKey(ReadIniValue(path, L"ToggleKey", L"F10"), VK_F10);
    config.disableFilter = Utils::ParseBoolean(
        ReadIniValue(path, L"DisableFilter", L"true"), true);
    config.disableMotionBlur = Utils::ParseBoolean(
        ReadIniValue(path, L"DisableMotionBlur", L"true"), true);

    spdlog::info("Config: enabled={}, toggleKey={:#x}, disableFilter={}, disableMotionBlur={}",
        config.enabled, config.toggleKey, config.disableFilter, config.disableMotionBlur);
    return true;
}
