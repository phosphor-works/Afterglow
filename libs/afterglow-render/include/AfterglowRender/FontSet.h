// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/afterglowrender_export.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;
typedef struct hb_font_t hb_font_t;
typedef struct hb_feature_t hb_feature_t;

namespace AfterglowRender {

/// One font style slot. Variable axes are applied when the font has them
/// (Monaspace Var: wght 200-800, wdth 100-125, slnt -11-0), so every slot
/// can point at a different family for font mixing.
struct FontStyleSpec
{
    std::string family;
    float weight = 400;
    float width = 100;
    float slant = 0;
    bool operator==(const FontStyleSpec&) const = default;
};

struct FontSpec
{
    float sizePoints = 11.0f;
    FontStyleSpec regular{"Monaspace Neon Var", 400, 100, 0};
    FontStyleSpec bold{"Monaspace Neon Var", 700, 100, 0};
    FontStyleSpec italic{"Monaspace Neon Var", 400, 100, -11};
    FontStyleSpec boldItalic{"Monaspace Neon Var", 700, 100, -11};
    /// OpenType features in HarfBuzz syntax: "calt", "liga", "ss01", "cv01=2", "-liga".
    /// calt carries Monaspace texture healing; ss01-ss10 are its coding ligature sets.
    std::vector<std::string> features{"calt", "liga", "ss01", "ss02", "ss03", "ss04", "ss05",
                                      "ss06", "ss07", "ss08", "ss09", "ss10"};
    /// Families tried, in order, for characters the style's own font lacks,
    /// before fontconfig's choice.
    std::vector<std::string> fallbackFamilies;
    /// Cell size against the font's own: 1.2 adds a fifth of a line between rows.
    float cellWidthScale = 1.0f;
    float cellHeightScale = 1.0f;
    bool operator==(const FontSpec&) const = default;
};

enum class StyleSlot : std::uint8_t { Regular, Bold, Italic, BoldItalic };

struct FontMetrics
{
    int cellWidth = 0;
    int cellHeight = 0;
    int baseline = 0; ///< from the cell top
    int underlinePosition = 0; ///< from the cell top, centre of the line
    int underlineThickness = 1;
    int strikethroughPosition = 0;
    /// Added to text glyphs' x so they stay centred in widened (or narrowed) cells.
    int glyphOffsetX = 0;
};

struct Face
{
    FT_Face ft = nullptr;
    hb_font_t* hb = nullptr;
    bool color = false;
    /// Bitmap-only fonts (colour emoji strikes) are scaled at rasterisation.
    bool scalable = true;
    std::string path;
};

/// Faces, metrics and fallback for one font size at one device pixel ratio.
/// Not thread-safe: used on the render (GUI) thread only.
class AFTERGLOWRENDER_EXPORT FontSet
{
public:
    FontSet(const FontSpec& spec, double devicePixelRatio);
    ~FontSet();

    FontSet(const FontSet&) = delete;
    FontSet& operator=(const FontSet&) = delete;

    const FontMetrics& metrics() const noexcept { return m_metrics; }
    const FontSpec& spec() const noexcept { return m_spec; }
    double devicePixelRatio() const noexcept { return m_dpr; }
    float pixelSize() const noexcept { return m_pixelSize; }

    /// Face index for a style slot (always 0-3).
    int primary(StyleSlot slot) const noexcept { return int(slot); }
    const Face& face(int index) const noexcept { return *m_faces[std::size_t(index)]; }
    int faceCount() const noexcept { return int(m_faces.size()); }

    /// Face to draw @p cp with in @p slot: the primary face when it has the
    /// glyph, otherwise a fontconfig fallback. @p preferColor asks for colour
    /// emoji (emoji presentation, VS16).
    int faceFor(char32_t cp, StyleSlot slot, bool preferColor);

    const std::vector<hb_feature_t>& features() const noexcept { return m_features; }

private:
    int loadFace(const std::string& path, int index, const FontStyleSpec* axes);
    int findFallback(char32_t cp, bool color);
    void computeMetrics();

    FontSpec m_spec;
    double m_dpr;
    float m_pixelSize;
    FT_Library m_ft = nullptr;
    std::vector<std::unique_ptr<Face>> m_faces;
    std::unordered_map<std::string, int> m_faceByPath;
    std::unordered_map<std::uint64_t, int> m_fallbackCache;
    std::vector<int> m_fallbackFaces; ///< the spec's fallback families, loaded
    std::vector<hb_feature_t> m_features;
    FontMetrics m_metrics;
};

} // namespace AfterglowRender
