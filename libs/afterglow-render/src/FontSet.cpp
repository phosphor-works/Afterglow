// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/FontSet.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include FT_MULTIPLE_MASTERS_H
#include FT_TRUETYPE_TABLES_H

#include <fontconfig/fontconfig.h>
#include <hb-ft.h>
#include <hb.h>

#include <QLoggingCategory>

#include <cmath>
#include <algorithm>
#include <atomic>
#include <stdexcept>

Q_LOGGING_CATEGORY(lcFont, "afterglow.font", QtInfoMsg)

namespace AfterglowRender {

namespace {

struct FcMatch
{
    std::string path;
    int index = 0;
    std::string family; ///< the matched font's first family name
    bool exact = false; ///< the font has the family asked for; otherwise fontconfig substituted
};

void matchStyle(FcPattern* pat, const FontStyleSpec& style)
{
    FcPatternAddDouble(pat, FC_WEIGHT, FcWeightFromOpenTypeDouble(style.weight));
    FcPatternAddDouble(pat, FC_WIDTH, style.width);
    FcPatternAddInteger(pat, FC_SLANT, style.slant == 0 ? FC_SLANT_ROMAN : FC_SLANT_ITALIC);
}

FcMatch matchFamily(const std::string& family, bool mono, const FontStyleSpec& style)
{
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
    matchStyle(pat, style);
    if (mono) {
        FcPatternAddInteger(pat, FC_SPACING, FC_MONO);
    }
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, pat, &result);
    FcMatch out;
    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
            out.path = reinterpret_cast<const char*>(file);
        }
        FcPatternGetInteger(match, FC_INDEX, 0, &out.index);
        FcChar8* name = nullptr;
        for (int i = 0; FcPatternGetString(match, FC_FAMILY, i, &name) == FcResultMatch; ++i) {
            if (i == 0) {
                out.family = reinterpret_cast<const char*>(name);
            }
            if (FcStrCmpIgnoreCase(name, reinterpret_cast<const FcChar8*>(family.c_str())) == 0) {
                out.exact = true;
            }
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return out;
}

/// fontconfig's aliases, which never name a font themselves.
bool isGenericFamily(const std::string& family)
{
    for (const char* g : {"monospace", "mono", "sans-serif", "sans", "serif", "emoji", "system-ui"}) {
        if (FcStrCmpIgnoreCase(reinterpret_cast<const FcChar8*>(family.c_str()), reinterpret_cast<const FcChar8*>(g)) == 0) {
            return true;
        }
    }
    return false;
}

void applyAxes(FT_Library lib, FT_Face face, const FontStyleSpec& spec)
{
    if (!FT_HAS_MULTIPLE_MASTERS(face)) {
        return;
    }
    FT_MM_Var* mm = nullptr;
    if (FT_Get_MM_Var(face, &mm) != 0) {
        return;
    }
    std::vector<FT_Fixed> coords(mm->num_axis);
    for (FT_UInt i = 0; i < mm->num_axis; ++i) {
        const FT_Var_Axis& a = mm->axis[i];
        double v = double(a.def) / 65536.0;
        switch (a.tag) {
        case FT_MAKE_TAG('w', 'g', 'h', 't'):
            v = spec.weight;
            break;
        case FT_MAKE_TAG('w', 'd', 't', 'h'):
            v = spec.width;
            break;
        case FT_MAKE_TAG('i', 't', 'a', 'l'):
            v = spec.slant == 0 ? 0 : 1;
            break;
        case FT_MAKE_TAG('s', 'l', 'n', 't'):
            v = spec.slant;
            break;
        default:
            break;
        }
        v = std::clamp(v, double(a.minimum) / 65536.0, double(a.maximum) / 65536.0);
        coords[i] = FT_Fixed(std::lround(v * 65536.0));
    }
    FT_Set_Var_Design_Coordinates(face, mm->num_axis, coords.data());
    FT_Done_MM_Var(lib, mm);
}

} // namespace

FontSet::FontSet(const FontSpec& spec, double devicePixelRatio)
    : m_identity([] { static std::atomic<std::uint64_t> serial{0}; return ++serial; }())
    , m_spec(spec)
    , m_dpr(devicePixelRatio)
    , m_pixelSize(float(spec.sizePoints * 96.0 / 72.0 * devicePixelRatio))
{
    if (FT_Init_FreeType(&m_ft) != 0) {
        throw std::runtime_error("FreeType initialisation failed");
    }
    try {
        // ClearType builds need filtering; Harmony builds use their own geometry
        // and return an unimplemented-feature error for this optional setting.
        FT_Library_SetLcdFilter(m_ft, FT_LCD_FILTER_DEFAULT);
        FcInit();

        const FontStyleSpec* styleSpecs[4] = {&m_spec.regular, &m_spec.bold, &m_spec.italic, &m_spec.boldItalic};
        for (int slot = 0; slot < 4; ++slot) {
            const FontStyleSpec* s = styleSpecs[slot];
            const FcMatch m = matchFamily(s->family, true, *s);
            if (m.path.empty()) {
                throw std::runtime_error("no font found for family " + s->family);
            }
            if (!m.exact && !isGenericFamily(s->family)) {
                qCWarning(lcFont) << "font family" << s->family.c_str() << "is not installed; using" << m.family.c_str();
            }
            // Each slot gets its own FT_Face so variable axes stay independent.
            const int idx = loadFace(m.path, m.index, s);
            if (idx < 0) {
                throw std::runtime_error("failed to load font " + m.path);
            }
            qCInfo(lcFont) << "slot" << slot << s->family.c_str() << "->" << m.path.c_str();
        }

        for (int slot = 0; slot < 4; ++slot) {
            const auto& style = *styleSpecs[slot];
            auto loadStyled = [&](const std::string& family) {
                const FcMatch match = matchFamily(family, false, style);
                if (!match.exact) {
                    qCWarning(lcFont) << "font family" << family.c_str() << "is not installed";
                    return -1;
                }
                const auto key = match.path + '#' + std::to_string(match.index) + '#' + std::to_string(slot);
                if (auto it = m_faceByPath.find(key); it != m_faceByPath.end()) return it->second;
                const int face = loadFace(match.path, match.index, &style);
                if (face >= 0) m_faceByPath.emplace(key, face);
                return face;
            };
            for (const auto& entry : m_spec.symbolMap) m_symbolFaces[slot].push_back(loadStyled(entry.family));
            for (const auto& family : m_spec.fallbackFamilies) {
                const int face = loadStyled(family);
                if (face >= 0) m_fallbackFaces[slot].push_back(face);
            }
        }

        for (const std::string& f : m_spec.features) {
            hb_feature_t feat;
            if (hb_feature_from_string(f.c_str(), int(f.size()), &feat)) {
                m_features.push_back(feat);
            } else {
                qCWarning(lcFont) << "ignoring invalid font feature" << f.c_str();
            }
        }

        computeMetrics();
    } catch (...) {
        releaseFaces();
        FT_Done_FreeType(m_ft);
        m_ft = nullptr;
        throw;
    }
}

FontSet::~FontSet()
{
    releaseFaces();
    FT_Done_FreeType(m_ft);
}

void FontSet::releaseFaces() noexcept
{
    for (auto& f : m_faces) {
        if (f->hb) {
            hb_font_destroy(f->hb);
        }
        if (f->ft) {
            FT_Done_Face(f->ft);
        }
    }
    m_faces.clear();
}

int FontSet::loadFace(const std::string& path, int index, const FontStyleSpec* axes)
{
    FT_Face ft = nullptr;
    if (FT_New_Face(m_ft, path.c_str(), index, &ft) != 0) {
        return -1;
    }
    const auto freeFace = [](FT_Face value) { FT_Done_Face(value); };
    std::unique_ptr<FT_FaceRec_, decltype(freeFace)> faceGuard(ft, freeFace);
    auto face = std::make_unique<Face>();
    face->ft = ft;
    face->path = path;
    face->color = FT_HAS_COLOR(ft);
    face->scalable = FT_IS_SCALABLE(ft);

    if (face->scalable) {
        FT_Set_Char_Size(ft, 0, FT_F26Dot6(std::lround(m_pixelSize * 64.0f)), 72, 72);
        if (axes) {
            applyAxes(m_ft, ft, *axes);
        }
    } else if (ft->num_fixed_sizes > 0) {
        // Pick the strike closest to the target size; the rasteriser scales it.
        int best = 0;
        for (int i = 1; i < ft->num_fixed_sizes; ++i) {
            if (std::abs(ft->available_sizes[i].height - m_pixelSize)
                < std::abs(ft->available_sizes[best].height - m_pixelSize)) {
                best = i;
            }
        }
        FT_Select_Size(ft, best);
    }
    face->hb = hb_ft_font_create_referenced(ft);
    std::unique_ptr<hb_font_t, decltype(&hb_font_destroy)> hbGuard(face->hb, &hb_font_destroy);
    hb_ft_font_set_load_flags(face->hb, loadFlags());

    m_faces.push_back(std::move(face));
    hbGuard.release();
    faceGuard.release();
    return int(m_faces.size()) - 1;
}

void FontSet::computeMetrics()
{
    FT_Face ft = m_faces[0]->ft;
    const FT_Size_Metrics& sm = ft->size->metrics;

    int advance = 0;
    if (FT_Load_Char(ft, 'M', loadFlags()) == 0) {
        advance = int(std::lround(double(ft->glyph->advance.x) / 64.0));
    }
    if (advance <= 0) {
        advance = int(std::lround(double(sm.max_advance) / 64.0));
    }

    const double ascender = double(sm.ascender) / 64.0;
    const double descender = double(sm.descender) / 64.0;
    const double height = double(sm.height) / 64.0;

    const int naturalWidth = std::max(1, advance);
    const int naturalHeight = std::max(1, int(std::lround(height)));
    m_metrics.cellWidth = std::max(1, int(std::lround(naturalWidth * m_spec.cellWidthScale)));
    m_metrics.cellHeight = std::max(1, int(std::lround(height * m_spec.cellHeightScale)));
    m_metrics.glyphOffsetX = (m_metrics.cellWidth - naturalWidth) / 2;
    // Split any line gap, and the room a taller cell adds, evenly above and
    // below the glyph box.
    const double gap = height - (ascender - descender);
    m_metrics.baseline = int(std::lround(ascender + gap / 2.0)) + (m_metrics.cellHeight - naturalHeight) / 2;

    const double yScale = double(sm.y_scale) / 65536.0 / 64.0;
    const double ulPos = -double(ft->underline_position) * yScale;
    const double ulThick = double(ft->underline_thickness) * yScale;
    m_metrics.underlineThickness = std::max(1, int(std::lround(ulThick)));
    m_metrics.underlinePosition = std::min(m_metrics.cellHeight - m_metrics.underlineThickness,
                                           m_metrics.baseline + int(std::lround(ulPos)));

    int strikePos = m_metrics.baseline - int(std::lround(ascender * 0.3));
    if (const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(ft, FT_SFNT_OS2)); os2 && os2->yStrikeoutPosition > 0) {
        strikePos = m_metrics.baseline - int(std::lround(double(os2->yStrikeoutPosition) * yScale));
    }
    m_metrics.strikethroughPosition = strikePos;
}

std::int32_t FontSet::loadFlags() const noexcept
{
    switch (m_spec.hinting) {
    case Hinting::None:
        return FT_LOAD_NO_HINTING;
    case Hinting::Full:
        return FT_LOAD_DEFAULT | (m_spec.antialiasing == Antialiasing::None ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_NORMAL);
    case Hinting::Light:
        break;
    }
    return FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT;
}

int FontSet::renderMode(bool allowSubpixel) const noexcept
{
    if (m_spec.antialiasing == Antialiasing::None) {
        return FT_RENDER_MODE_MONO;
    }
    if (allowSubpixel && subpixel()) {
        return FT_RENDER_MODE_LCD;
    }
    return m_spec.hinting == Hinting::Light ? FT_RENDER_MODE_LIGHT : FT_RENDER_MODE_NORMAL;
}

int FontSet::faceFor(char32_t cp, StyleSlot slot, bool preferColor)
{
    const int primaryIdx = int(slot);
    if (cp == 0 || cp == U' ') {
        return primaryIdx;
    }
    for (std::size_t i = 0; i < m_symbolFaces[primaryIdx].size(); ++i) {
        const SymbolMapEntry& entry = m_spec.symbolMap[i];
        const int idx = m_symbolFaces[primaryIdx][i];
        if (cp >= entry.first && cp <= entry.last && idx >= 0
            && FT_Get_Char_Index(m_faces[std::size_t(idx)]->ft, cp) != 0) {
            return idx;
        }
    }
    if (!preferColor && FT_Get_Char_Index(m_faces[std::size_t(primaryIdx)]->ft, cp) != 0) {
        return primaryIdx;
    }
    const std::uint64_t key = (std::uint64_t(cp) << 3) | (std::uint64_t(primaryIdx) << 1) | (preferColor ? 1 : 0);
    if (auto it = m_fallbackCache.find(key); it != m_fallbackCache.end()) {
        return it->second < 0 ? primaryIdx : it->second;
    }
    int idx = findFallback(cp, preferColor, slot);
    if (idx < 0 && preferColor && FT_Get_Char_Index(m_faces[std::size_t(primaryIdx)]->ft, cp) != 0) {
        idx = primaryIdx;
    }
    if (idx < 0 && preferColor) {
        idx = findFallback(cp, false, slot);
    }
    m_fallbackCache.emplace(key, idx);
    return idx < 0 ? primaryIdx : idx;
}

int FontSet::findFallback(char32_t cp, bool color, StyleSlot slot)
{
    const FontStyleSpec* styles[] = {&m_spec.regular, &m_spec.bold, &m_spec.italic, &m_spec.boldItalic};
    const auto& style = *styles[int(slot)];
    for (int idx : m_fallbackFaces[int(slot)]) {
        const Face& f = *m_faces[std::size_t(idx)];
        if ((!color || f.color) && FT_Get_Char_Index(f.ft, cp) != 0) {
            return idx;
        }
    }
    FcPattern* pat = FcPatternCreate();
    FcCharSet* cs = FcCharSetCreate();
    FcCharSetAddChar(cs, cp);
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    if (color) {
        FcPatternAddBool(pat, FC_COLOR, FcTrue);
    }
    FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>(style.family.c_str()));
    matchStyle(pat, style);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcFontSet* set = FcFontSort(nullptr, pat, FcTrue, nullptr, &result);
    int found = -1;
    if (set) {
        for (int i = 0; i < set->nfont && found < 0; ++i) {
            FcCharSet* fcs = nullptr;
            if (FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &fcs) != FcResultMatch || !FcCharSetHasChar(fcs, cp)) {
                continue;
            }
            FcBool isColor = FcFalse;
            FcPatternGetBool(set->fonts[i], FC_COLOR, 0, &isColor);
            if (color && !isColor) {
                continue;
            }
            // Bitmap fonts (PCF and friends) cannot shape complex scripts or
            // scale; only colour emoji strikes are worth a bitmap fallback.
            FcBool scalable = FcTrue;
            FcPatternGetBool(set->fonts[i], FC_SCALABLE, 0, &scalable);
            if (!scalable && !isColor) {
                continue;
            }
            FcChar8* file = nullptr;
            int index = 0;
            if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch) {
                continue;
            }
            FcPatternGetInteger(set->fonts[i], FC_INDEX, 0, &index);
            const std::string path = reinterpret_cast<const char*>(file);
            const std::string key = path + '#' + std::to_string(index) + '#' + std::to_string(int(slot));
            if (auto it = m_faceByPath.find(key); it != m_faceByPath.end()) {
                found = it->second;
            } else {
                found = loadFace(path, index, &style);
                if (found >= 0) {
                    m_faceByPath.emplace(key, found);
                }
            }
        }
        FcFontSetDestroy(set);
    }
    FcCharSetDestroy(cs);
    FcPatternDestroy(pat);
    return found;
}

} // namespace AfterglowRender
