#pragma once

#include <raylib.h>

// System sans font with Cyrillic glyphs, rasterised large and mipmapped so it
// stays smooth at any drawn size. Falls back to raylib's built-in font, which
// has no Cyrillic, when no candidate font file exists.
class UiFont {
public:
    // Requires an initialised window (an OpenGL context).
    UiFont();
    ~UiFont();
    UiFont(const UiFont&) = delete;
    UiFont& operator=(const UiFont&) = delete;

    const Font& get() const { return font_; }

private:
    Font font_{};
    bool loaded_ = false;
};
