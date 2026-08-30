#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace Utils
{
    struct ExecutableSection
    {
        uint8_t* begin = nullptr;
        uintptr_t size = 0;
    };

    std::filesystem::path GetModulePath(HMODULE module);
    bool FindModuleSection(HMODULE module, const char* name, ExecutableSection& result);
    uint8_t* PatternScanRange(void* begin, uintptr_t size, const char* signature);
    uintptr_t ResolveRelativeAddress(const uint8_t* displacement);
    bool ParseBoolean(std::wstring value, bool fallback);
    void LogAddress(const char* name, uintptr_t address, uintptr_t moduleBase);
}
