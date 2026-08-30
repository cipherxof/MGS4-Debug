#include "utils.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <spdlog/spdlog.h>
#include <vector>

namespace
{
    std::vector<int> PatternToBytes(const char* pattern)
    {
        std::vector<int> bytes;
        const char* current = pattern;
        const char* end = pattern + std::strlen(pattern);

        while (current < end)
        {
            if (*current == ' ')
            {
                ++current;
                continue;
            }

            if (*current == '?')
            {
                ++current;
                if (current < end && *current == '?')
                    ++current;
                bytes.push_back(-1);
                continue;
            }

            char* next = nullptr;
            bytes.push_back(static_cast<int>(std::strtoul(current, &next, 16)));
            if (next == current)
                return {};
            current = next;
        }
        return bytes;
    }
}

std::filesystem::path Utils::GetModulePath(HMODULE module)
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return std::filesystem::path(path);
}

bool Utils::FindModuleSection(HMODULE module, const char* name, ExecutableSection& result)
{
    result = {};
    if (!module || !name)
        return false;

    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(ntHeaders);
    for (WORD index = 0; index < ntHeaders->FileHeader.NumberOfSections; ++index, ++section)
    {
        if (std::strncmp(reinterpret_cast<const char*>(section->Name), name, IMAGE_SIZEOF_SHORT_NAME) != 0)
            continue;

        result.begin = reinterpret_cast<uint8_t*>(module) + section->VirtualAddress;
        result.size = static_cast<uintptr_t>(section->Misc.VirtualSize);
        return result.size != 0;
    }
    return false;
}

uint8_t* Utils::PatternScanRange(void* begin, uintptr_t size, const char* signature)
{
    if (!begin || !signature)
        return nullptr;

    const std::vector<int> pattern = PatternToBytes(signature);
    if (pattern.empty() || size < pattern.size())
        return nullptr;

    auto* bytes = static_cast<uint8_t*>(begin);
    for (uintptr_t offset = 0; offset <= size - pattern.size(); ++offset)
    {
        bool matches = true;
        for (size_t index = 0; index < pattern.size(); ++index)
        {
            if (pattern[index] != -1 && bytes[offset + index] != static_cast<uint8_t>(pattern[index]))
            {
                matches = false;
                break;
            }
        }
        if (matches)
            return bytes + offset;
    }
    return nullptr;
}

uintptr_t Utils::ResolveRelativeAddress(const uint8_t* displacement)
{
    int32_t relative = 0;
    std::memcpy(&relative, displacement, sizeof(relative));
    return reinterpret_cast<uintptr_t>(displacement) + sizeof(relative) + relative;
}

bool Utils::ParseBoolean(std::wstring value, bool fallback)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });

    if (value == L"true" || value == L"1" || value == L"yes" || value == L"on")
        return true;
    if (value == L"false" || value == L"0" || value == L"no" || value == L"off")
        return false;
    return fallback;
}

void Utils::LogAddress(const char* name, uintptr_t address, uintptr_t moduleBase)
{
    if (address < moduleBase)
    {
        spdlog::error("Failed to resolve address {}", name);
        return;
    }
    spdlog::debug("{} = {:#x}", name, address - moduleBase);
}
