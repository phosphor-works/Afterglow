// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later
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
    // netPBM files are text up to their samples.
    const std::string netpbm[] = {
        "P1\n3 2\n1 0 1\n010",
        "P2\n3 1\n15\n0 7 15\n",
        "P3 1 1 255\n10 20 30\n",
        std::string("P4\n10 1\n\x90\x40", 10),
        std::string("P5 2 1 65535\n\x80\x00\xff\xff", 17),
        "P6\n2 1\n255\n\x01\x02\x03\xfa\xfb\xfc",
        "P7\nWIDTH 1\nHEIGHT 1\nDEPTH 2\nMAXVAL 3\nENDHDR\n\x01\x03",
        "P7\nWIDTH 2\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n"
            + std::string("\x01\x02\x03\x80\x04\x05\x06\x00", 8),
    };
    const std::pair<const char*, const std::string*> files[] = {
        {"rgba.png", &Png},
        {"grey16.png", &Png16},
        {"grey16-mid.png", &Png16Mid},
        {"palette.png", &PngPalette},
        {"gamma.png", &PngGamma},
        {"grey-alpha.png", &PngGreyAlpha},
        {"rgb.png", &PngRgb},
        {"rgba16.png", &PngRgba16},
        {"grey2.png", &PngGrey2},
        {"grey-trns.png", &PngGreyTrns},
        {"rgb-trns.png", &PngRgbTrns},
        {"interlaced.png", &PngInterlaced},
        {"adobe.png", &PngAdobe},
        {"cicp-p3.png", &PngCicpP3},
        {"cicp-pq.png", &PngCicpPq},
        {"bad-icc.png", &PngBadIcc},
        {"rgb.bmp", &Bmp},
        {"two-frames.gif", &Gif},
        {"rgb.jpg", &Jpeg},
        {"cmyk.jpg", &JpegCmyk},
        {"cmyk-dark.jpg", &JpegCmykDark},
        {"progressive.jpg", &JpegProgressive},
        {"grey.jpg", &JpegGrey},
        {"adobe.jpg", &JpegAdobe},
        {"lossless.webp", &Webp},
        {"lossy.webp", &WebpLossy},
        {"alpha.webp", &WebpAlpha},
        {"lossy-alpha.webp", &WebpLossyAlpha},
        {"two-frames.webp", &WebpAnimated},
        {"adobe.webp", &WebpAdobe},
        {"rgb.tif", &TiffRgb},
        {"lzw.tif", &TiffLzw},
        {"deflate-predictor.tif", &TiffDeflatePredictor},
        {"packbits-big.tif", &TiffPackBitsBig},
        {"rgb16-predictor.tif", &Tiff16Predictor},
        {"bilevel.tif", &TiffBilevel},
        {"palette.tif", &TiffPalette},
        {"tiled.tif", &TiffTiled},
        {"rgba.tif", &TiffRgba},
        {"cmyk.tif", &TiffCmyk},
        {"adobe.tif", &TiffAdobe},
        {"three-frames.png", &Apng},
        {"bad-sequence.png", &ApngBadSequence},
        {"hidden-default.png", &ApngHiddenDefault},
        {"plain.pbm", &netpbm[0]},
        {"plain.pgm", &netpbm[1]},
        {"plain.ppm", &netpbm[2]},
        {"raw.pbm", &netpbm[3]},
        {"wide.pgm", &netpbm[4]},
        {"raw.ppm", &netpbm[5]},
        {"grey-alpha.pam", &netpbm[6]},
        {"rgba.pam", &netpbm[7]},
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
