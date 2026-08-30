#pragma once

#include <windows.h>

#include <filesystem>

struct DebugConfig
{
    bool enabled = true;
    int toggleKey = VK_F10;
    bool disableFilter = true;
    bool disableMotionBlur = true;
};

bool LoadConfig(const std::filesystem::path& path, DebugConfig& config);
