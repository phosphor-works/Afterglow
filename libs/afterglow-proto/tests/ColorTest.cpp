// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// OKLCH and CIE LAB colours. The expected values come from the sample code
// and the gamut mapping of CSS Color 4, which give the example colour of the
// specification exactly. kitty 0.50 agrees on OKLCH inside sRGB. Its LAB
// conversion differs, and the tests say where.

#include <AfterglowProto/Color.h>

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace {

using Rgb = std::array<int, 3>;

/// The colour of @p spec, or {-1, -1, -1} when it is refused.
Rgb parsed(const std::string& spec)
{
    std::uint8_t r = 1, g = 2, b = 3;
    if (!AfterglowProto::parseWideGamutColor(spec, r, g, b)) {
        EXPECT_EQ(Rgb({r, g, b}), Rgb({1, 2, 3})) << spec << ": a refused colour changes nothing";
        return {-1, -1, -1};
    }
    return {r, g, b};
}

constexpr Rgb Refused{-1, -1, -1};

} // namespace

TEST(WideGamutColor, MatchesTheExampleOfCssColor4)
{
    // CSS Color 4 writes #7D2329 as both of these.
    EXPECT_EQ(parsed("lab(29.2345% 39.3825 20.0664)"), Rgb({125, 35, 41}));
    EXPECT_EQ(parsed("oklch(40.101% 0.12332 21.555)"), Rgb({125, 35, 41}));
}

TEST(WideGamutColor, ColorsInsideSrgbMatchKitty)
{
    EXPECT_EQ(parsed("oklch(0.9 0.05 140)"), Rgb({205, 231, 199}));
    EXPECT_EQ(parsed("oklch(70% 0.1 200)"), Rgb({64, 177, 183}));
    EXPECT_EQ(parsed("oklch(0.5 0.1 100)"), Rgb({113, 100, 11}));
}

TEST(WideGamutColor, ColorsOutsideSrgbAreMappedAndNotClipped)
{
    // kitty 0.50 clips each channel: (255, 61, 70) here, lightness 0.66
    // where the mapping keeps 0.68 of 0.7, and pure red, hue 29, for C 0.4.
    EXPECT_EQ(parsed("oklch(0.7 0.25 25)"), Rgb({255, 86, 85}));
    EXPECT_EQ(parsed("oklch(0.7 0.4 25)"), Rgb({255, 86, 85}));
    // kitty: (239, 233, 0).
    EXPECT_EQ(parsed("oklch(0.9 0.3 110)"), Rgb({232, 232, 0}));
    // kitty clips to the same colour here: the clipped colour is less than
    // one just noticeable difference away, so CSS keeps it too.
    EXPECT_EQ(parsed("oklch(0.5 0.3 264)"), Rgb({0, 54, 255}));
    // kitty: pure red.
    EXPECT_EQ(parsed("lab(50 100 100)"), Rgb({245, 0, 13}));
}

TEST(WideGamutColor, LabFollowsCssWhereKittyDiffers)
{
    // kitty 0.50 gives (130, 130, 130) for what its own documentation calls
    // very dark, and (245, 245, 245) for lab(90 0 0).
    EXPECT_EQ(parsed("lab(10 0 0)"), Rgb({27, 27, 27}));
    EXPECT_EQ(parsed("lab(90 0 0)"), Rgb({226, 226, 226}));
    EXPECT_EQ(parsed("lab(50 60 40)"), Rgb({214, 60, 55}));
    EXPECT_EQ(parsed("lab(50 0 -50)"), Rgb({54, 122, 205}));
}

TEST(WideGamutColor, ValuesOutsideTheirRangesAreBroughtIn)
{
    EXPECT_EQ(parsed("oklch(0 0 0)"), Rgb({0, 0, 0}));
    EXPECT_EQ(parsed("oklch(1 0 0)"), Rgb({255, 255, 255}));
    EXPECT_EQ(parsed("oklch(1.5 0.1 20)"), Rgb({255, 255, 255}));
    EXPECT_EQ(parsed("oklch(-0.1 0.1 20)"), Rgb({0, 0, 0}));
    // A negative chroma is 0, as in CSS: a gray.
    EXPECT_EQ(parsed("oklch(0.7 -0.1 20)"), Rgb({158, 158, 158}));
    EXPECT_EQ(parsed("oklch(0.7 0 0)"), Rgb({158, 158, 158}));
    EXPECT_EQ(parsed("lab(0 0 0)"), Rgb({0, 0, 0}));
    EXPECT_EQ(parsed("lab(100 0 0)"), Rgb({255, 255, 255}));
    EXPECT_EQ(parsed("lab(120 0 0)"), Rgb({255, 255, 255}));
    EXPECT_EQ(parsed("lab(-5 0 0)"), Rgb({0, 0, 0}));
    // The hue goes round.
    EXPECT_EQ(parsed("oklch(0.5 0.1 460)"), Rgb({113, 100, 11}));
    EXPECT_EQ(parsed("oklch(0.5 0.1 -260)"), Rgb({113, 100, 11}));
}

TEST(WideGamutColor, AHugeChromaIsBounded)
{
    // CSS gives (116, 99, 0) for C 1e100 too. Past 1e104 the cubes of the
    // conversion overflow, and NaN must not count as inside sRGB (it gave
    // black).
    EXPECT_EQ(parsed("oklch(0.5 0.4 100)"), Rgb({116, 99, 0}));
    EXPECT_EQ(parsed("oklch(0.5 1e200 100)"), Rgb({116, 99, 0}));
    EXPECT_EQ(parsed("oklch(0.5 1.7e308 100)"), Rgb({116, 99, 0}));
    // a and b of LAB keep their ratio. 1.5e308% is past the largest double,
    // and gave a mid gray where 1.4e308% gave black.
    const Rgb huge = parsed("lab(50 1e6 0)");
    EXPECT_EQ(huge, Rgb({255, 255, 255}));
    for (const char* spec : {"lab(50 1e308 0)", "lab(50 1.4e308% 0)", "lab(50 1.5e308% 0)"}) {
        EXPECT_EQ(parsed(spec), huge) << spec;
    }
    // Inside the documented range nothing is bounded: this is C 1.93 in OKLCH.
    EXPECT_EQ(parsed("lab(50 -125 -125)"), Rgb({0, 93, 90}));
    EXPECT_EQ(parsed("lab(50 -100% -100%)"), Rgb({0, 93, 90}));
}

TEST(WideGamutColor, ReadsTheSpellingsOfKittyAndCss)
{
    const Rgb want{113, 100, 11};
    for (const char* spec : {"OKLCH(0.5 0.1 100)", "Oklch(0.5,0.1,100)", "oklch( 0.5  0.1  100 )", "  oklch(0.5 0.1 100)\t",
                             "oklch(.5 .1 1e2)", "oklch(+0.5 0.1 100)", "oklch(50% 0.1 100)", "oklch(0.5, 0.1, 100)",
                             "oklch(0.5 0.1 100deg)", "oklch(0.5 0.1 100DEG)",
                             // Empty fields between commas, as kitty reads them.
                             "oklch(,0.5,,0.1 100,)"}) {
        EXPECT_EQ(parsed(spec), want) << spec;
    }
    // The angle units of CSS.
    const Rgb ninety = parsed("oklch(0.5 0.1 90)");
    EXPECT_EQ(ninety, Rgb({121, 96, 6}));
    EXPECT_EQ(parsed("oklch(0.5 0.1 100grad)"), ninety);
    EXPECT_EQ(parsed("oklch(0.5 0.1 0.25turn)"), ninety);
    EXPECT_EQ(parsed("oklch(0.5 0.1 1.5707963267948966rad)"), ninety);
    // "none" is 0.
    EXPECT_EQ(parsed("oklch(0.7 none 0)"), Rgb({158, 158, 158}));
    EXPECT_EQ(parsed("oklch(0.7 0 none)"), Rgb({158, 158, 158}));
    // Percentages as in CSS: C 100% is 0.4, and a and b 100% are 125. kitty
    // reads a percentage of C as one of 1 (40% is 0.4), and a and b with the
    // % ignored.
    EXPECT_EQ(parsed("oklch(0.7 40% 25)"), parsed("oklch(0.7 0.16 25)"));
    EXPECT_EQ(parsed("oklch(0.7 0.16 25)"), Rgb({242, 113, 106}));
    EXPECT_EQ(parsed("lab(50% 60% 32%)"), parsed("lab(50 75 40)"));
    EXPECT_EQ(parsed("lab(50 75 40)"), Rgb({232, 12, 58}));
}

TEST(WideGamutColor, AnAlphaIsReadAndIgnored)
{
    // As kitty 0.50 does: terminal colours have no alpha.
    const Rgb want{113, 100, 11};
    for (const char* spec : {"oklch(0.5 0.1 100 / 0.5)", "oklch(0.5 0.1 100/0.5)", "oklch(0.5 0.1 100 / 50%)",
                             "oklch(0.5 0.1 100 / none)", "oklch(0.5 0.1 100 0.5)", "oklch(0.5,0.1,100,0.5)"}) {
        EXPECT_EQ(parsed(spec), want) << spec;
    }
    EXPECT_EQ(parsed("lab(50 60 40 / 0)"), parsed("lab(50 60 40)"));
}

TEST(WideGamutColor, RefusesWhatIsNoColor)
{
    for (const char* spec :
         {"oklch(0.7 0.25)", "oklch(0.7 0.25 25 /)", "oklch(0.7 0.25 / 25)", "oklch(0.7 0.25 25 / 0.5 / 1)",
          "oklch(0.7 0.25 25 0.5 0.5)", "oklch(0.7 0.25 25 / 1deg)", "oklch(0.7 0.25 25 / x)", "oklch(/ 0.7 0.25 25)",
          "oklch(0.7 0.25 +-25)", "oklch(0.7 0.25 25)x", "oklch(0.7 0.25 25", "oklch()", "oklch", "oklch (0.7 0.25 25)", "oklab(0.5 0.1 0.1)",
          "lch(50 30 20)", "oklch(0.7 0.25 25%)", "oklch(0.7deg 0.25 25)", "oklch(0.7 0.25deg 25)", "lab(50 60deg 40)",
          "oklch(0.7 0.25 25px)", "oklch(nan 0 0)", "oklch(inf 0 0)", "oklch(1e999 0 0)",
          "oklch(0.7 0.25 --25)", "oklch(0.7 0.25 ++25)", "lab(a b c)", "(0.7 0.25 25)"}) {
        EXPECT_EQ(parsed(spec), Refused) << spec;
    }
    EXPECT_EQ(parsed("oklch(" + std::string(300, ' ') + "0.5 0.1 100)"), Refused) << "longer than any colour";
}

TEST(WideGamutColor, ParseColorSpecTakesThem)
{
    std::uint8_t r = 0, g = 0, b = 0;
    ASSERT_TRUE(AfterglowProto::parseColorSpec("oklch(0.9 0.05 140)", r, g, b));
    EXPECT_EQ(Rgb({r, g, b}), Rgb({205, 231, 199}));
    ASSERT_TRUE(AfterglowProto::parseColorSpec("LAB(10 0 0)", r, g, b));
    EXPECT_EQ(Rgb({r, g, b}), Rgb({27, 27, 27}));
    // The X11 forms still read as before.
    ASSERT_TRUE(AfterglowProto::parseColorSpec("rgb:ff/80/00", r, g, b));
    EXPECT_EQ(Rgb({r, g, b}), Rgb({255, 128, 0}));
    EXPECT_FALSE(AfterglowProto::parseColorSpec("oklch(0.9 0.05)", r, g, b));
    EXPECT_EQ(Rgb({r, g, b}), Rgb({255, 128, 0}));
}
