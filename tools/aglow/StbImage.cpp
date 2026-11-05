// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The implementation of the vendored stb_image, in its own target: it builds
// without Afterglow's warnings, as zlib-ng and xxHash do. aglow decodes from
// memory only, and refuses images larger than any terminal shows.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS (1 << 15)
#include <stb_image.h>
