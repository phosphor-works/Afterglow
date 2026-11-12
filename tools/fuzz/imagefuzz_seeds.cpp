// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// afterglow_imagefuzz_seeds DIRECTORY
//
// Writes the image files of the tests of libs/afterglow-image into
// DIRECTORY, as a first corpus for afterglow_imagefuzz: a valid file of each
// format and kind, which a fuzzer does not find from nothing (a PNG chunk
// has a checksum, and a JPEG has Huffman tables).

#include "ImageFiles.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <utility>

int main(int argc, char** argv)
{
    using namespace AfterglowImage::TestFiles;
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s DIRECTORY\n", argv[0]);
        return 2;
    }
    const std::pair<const char*, const std::string*> files[] = {
        {"rgba.png", &Png}, {"grey16.png", &Png16}, {"grey16-mid.png", &Png16Mid}, {"palette.png", &PngPalette},
        {"gamma.png", &PngGamma}, {"grey-alpha.png", &PngGreyAlpha}, {"rgb.png", &PngRgb}, {"rgba16.png", &PngRgba16},
        {"grey2.png", &PngGrey2}, {"grey-trns.png", &PngGreyTrns}, {"rgb-trns.png", &PngRgbTrns}, {"interlaced.png", &PngInterlaced},
        {"rgb.bmp", &Bmp}, {"two-frames.gif", &Gif}, {"rgb.jpg", &Jpeg}, {"cmyk.jpg", &JpegCmyk},
        {"cmyk-dark.jpg", &JpegCmykDark}, {"progressive.jpg", &JpegProgressive}, {"grey.jpg", &JpegGrey},
        {"lossless.webp", &Webp}, {"lossy.webp", &WebpLossy}, {"alpha.webp", &WebpAlpha}, {"lossy-alpha.webp", &WebpLossyAlpha},
        {"two-frames.webp", &WebpAnimated},
    };
    for (const auto& [name, bytes] : files) {
        const std::string path = std::string(argv[1]) + '/' + name;
        std::ofstream out(path, std::ios::binary);
        out.write(bytes->data(), std::streamsize(bytes->size()));
        if (!out.flush()) {
            std::fprintf(stderr, "%s: cannot write %s\n", argv[0], path.c_str());
            return 1;
        }
    }
    return 0;
}
