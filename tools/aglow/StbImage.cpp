// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The implementation of the vendored stb_image, in its own target: it builds
// without Afterglow's warnings, as zlib-ng and xxHash do. aglow decodes from
// memory only. Before it decodes, aglow checks the size in the header
// against AfterglowProto::GraphicsMaxSide (the longest side that Afterglow
// takes) and the image memory limit, and reports which one an image breaks.
// stb_image's own limit is only a backstop past both: an image whose side is
// longer than 65,536 pixels is not read at all.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS (1 << 16)
#include <stb_image.h>
