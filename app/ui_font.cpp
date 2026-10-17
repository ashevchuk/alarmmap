#include "ui_font.hpp"

#include <filesystem>
#include <iostream>
#include <vector>

namespace {

constexpr const char* kCandidateFiles[] = {
    "/usr/share/fonts/noto/NotoSans-Medium.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
};
constexpr int kRasterSizePx = 64;

std::vector<int> uiCodepoints() {
    std::vector<int> codepoints;
    const auto addRange = [&](int first, int last) {
        for (int codepoint = first; codepoint <= last; ++codepoint) codepoints.push_back(codepoint);
    };
    addRange(0x20, 0x7E);    // Basic Latin
    addRange(0xA0, 0xFF);    // Latin-1: « » · ×
    addRange(0x400, 0x4FF);  // Cyrillic
    for (const int codepoint : {0x2013, 0x2014, 0x2019, 0x2026}) codepoints.push_back(codepoint);
    return codepoints;
}

}  // namespace

UiFont::UiFont() {
    for (const char* file : kCandidateFiles) {
        if (!std::filesystem::exists(file)) continue;
        std::vector<int> codepoints = uiCodepoints();
        font_ = LoadFontEx(file, kRasterSizePx, codepoints.data(), static_cast<int>(codepoints.size()));
        if (!IsFontValid(font_)) continue;
        GenTextureMipmaps(&font_.texture);
        SetTextureFilter(font_.texture, TEXTURE_FILTER_TRILINEAR);
        loaded_ = true;
        return;
    }
    std::cerr << "alarmmap: no font with Cyrillic found, labels will not render properly\n";
    font_ = GetFontDefault();
}

UiFont::~UiFont() {
    if (loaded_) UnloadFont(font_);
}
