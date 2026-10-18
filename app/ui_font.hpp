#pragma once

#include <raylib.h>

#include <filesystem>

// Sans font with Cyrillic glyphs, bundled or from the system, rasterised large and mipmapped so it
// stays smooth at any drawn size. Falls back to raylib's built-in font, which
// has no Cyrillic, when no candidate font file exists.
class UiFont {
public:
    // Requires an initialised window (an OpenGL context). Fonts in
    // `bundledFonts` take precedence over system ones.
    explicit UiFont(const std::filesystem::path& bundledFonts);
    ~UiFont();
    UiFont(const UiFont&) = delete;
    UiFont& operator=(const UiFont&) = delete;

    const Font& get() const { return font_; }

private:
    Font font_{};
    bool loaded_ = false;
};
