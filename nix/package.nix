# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Afterglow package: afterglow, aglow and aglow-mux, the shader packs,
# themes, shell integration, terminfo and desktop files, and the Phosphor
# libraries it is built with, from the phosphor-libs commit that
# cmake/AfterglowDependencies.cmake pins. `withTests` builds and runs the test
# suite in the sandbox, which is what the flake's `checks.<system>.tests` does.
# The sandbox has no Wayland display, so the tests that draw with Vulkan report
# Skipped, as docs/TESTING.md says.
{
  lib,
  stdenv,
  fetchurl,
  linkFarm,
  cmake,
  ninja,
  pkg-config,
  nasm,
  librsvg,
  ncurses,
  python3,
  qt6,
  kdePackages,
  wayland,
  wayland-scanner,
  vulkan-headers,
  freetype,
  harfbuzz,
  fribidi,
  fontconfig,
  libxkbcommon,
  pcre2,
  gtest,
  glibc,
  src,
  version,
  withTests ? false,
}:

let
  # The archives that cmake/AfterglowDependencies.cmake pins, and the Luau and
  # valijson archives that the pinned phosphor-libs commit pins in its
  # cmake/PhosphorDependencies.cmake. The sandbox has no network, so they are
  # fetched here as fixed-output derivations and handed to CMake as one
  # directory (AFTERGLOW_DEPS_ARCHIVES, which also sets PHOSPHOR_DEPS_ARCHIVES),
  # each under the FILE name of its pin. CMake checks each against the pin's
  # hash again before it unpacks it. fetchurl, not fetchFromGitHub: the pin
  # hashes the archive file itself. A pin that changes there changes here too.
  depsArchives = linkFarm "afterglow-deps-archives" [
    {
      name = "phosphor-libs-c5d6ee8a24f803607b403f90b8625b94b8a25110.tar.gz";
      path = fetchurl {
        url = "https://github.com/phosphor-works/phosphor-libs/archive/c5d6ee8a24f803607b403f90b8625b94b8a25110.tar.gz";
        # sha256 f072437c31f9b96e5ed43c32e55bc0e6e8c7ac7323262dbeaf871008c670e8e6
        hash = "sha256-8HJDfDH5uW5e1Dwy5VvA5ujHrHMjJi2+r4cQCMZw6OY=";
      };
    }
    {
      name = "zlib-ng-2.3.3.tar.gz";
      path = fetchurl {
        url = "https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz";
        # sha256 f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1
        hash = "sha256-+cZaqchS64JVtjb9nwfOHEBvBh7Bmi59UIsxjKDJB9E=";
      };
    }
    {
      name = "xxHash-0.8.4.tar.gz";
      path = fetchurl {
        url = "https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz";
        # sha256 5738270935e7c3d38a79b3adf7c9692566ce7895a25f67de43ad52ab504acd32
        hash = "sha256-VzgnCTXnw9OKebOt98lpJWbOeJWiX2feQ61Sq1BKzTI=";
      };
    }
    {
      name = "libpng-1.6.59.tar.xz";
      path = fetchurl {
        url = "https://downloads.sourceforge.net/project/libpng/libpng16/1.6.59/libpng-1.6.59.tar.xz";
        # sha256 d80dd2a38a37f803cb9b6ac7b14bd6e74ddc3b654780a8380bdf93523fdb4389
        hash = "sha256-2A3So4o3+APLm2rHsUvW503cO2VHgKg4C9+TUj/bQ4k=";
      };
    }
    {
      name = "libjpeg-turbo-3.2.1.tar.gz";
      path = fetchurl {
        url = "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.1/libjpeg-turbo-3.2.1.tar.gz";
        # sha256 c27ad72a68da151177929aa969ae773a2da5ac2af9ce946079314fdac8cece7f
        hash = "sha256-wnrXKmjaFRF3kpqpaa53Oi2lrCr5zpRgeTFP2sjOzn8=";
      };
    }
    {
      name = "libwebp-1.6.0.tar.gz";
      path = fetchurl {
        url = "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz";
        # sha256 e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
        hash = "sha256-5KtwCb8GKf0RmC1MKqg5ZM8kTP+6c0fs05AZqeOMRWQ=";
      };
    }
    {
      name = "lcms2-2.20.tar.gz";
      path = fetchurl {
        url = "https://github.com/mm2/Little-CMS/releases/download/lcms2.20/lcms2-2.20.tar.gz";
        # sha256 18ff5504fa9cc8a2b1f3591180d90ead16c8a2ea78e530523b46eab70e1ca6b9
        hash = "sha256-GP9VBPqcyKKx81kRgNkOrRbIoup45TBSO0bqtw4cprk=";
      };
    }
    {
      name = "luau-0.745.tar.gz";
      path = fetchurl {
        url = "https://github.com/luau-lang/luau/archive/refs/tags/0.745.tar.gz";
        # sha256 53c22773bace6cabbabaead576fbc08d11a0317fce11a3dbf9d027dda6849248
        hash = "sha256-U8Inc7rObKu6uurVdvvAjRGgMX/OEaPb+dAn3aaEkkg=";
      };
    }
    {
      name = "valijson-1.1.3.tar.gz";
      path = fetchurl {
        url = "https://github.com/tristanpenman/valijson/archive/refs/tags/v1.1.3.tar.gz";
        # sha256 887b53b1a924f6fe0b35fa3bbc9bbbe5ae8c72097b7f0f7c17b45f4cfa646029
        hash = "sha256-iHtTsakk9v4LNfo7vJu75a6Mcgl7fw98F7RfTPpkYCk=";
      };
    }
  ];
in
stdenv.mkDerivation {
  pname = if withTests then "afterglow-tests" else "afterglow";
  inherit version src;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    wayland-scanner
    # The SIMD code of libjpeg-turbo.
    nasm
    # rsvg-convert renders the PNG icon sizes, tic compiles the terminfo entry.
    librsvg
    ncurses
    # Wraps afterglow so it finds every Qt input's plugins and QML modules, and
    # collects those directories into qtWrapperArgs, which preCheck turns into
    # the test environment.
    qt6.wrapQtAppsHook
  ]
  # The Python tests in CTest, and the reference generator.
  ++ lib.optional withTests python3;

  buildInputs = [
    qt6.qtbase
    qt6.qtdeclarative
    qt6.qtshadertools
    qt6.qtsvg
    qt6.qtwayland
    kdePackages.kwindowsystem
    wayland
    # QRhi declares its Vulkan backend only with the Vulkan headers present.
    vulkan-headers
    freetype
    harfbuzz
    fribidi
    fontconfig
    libxkbcommon
    pcre2
    # libc.a, for the static aglow.
    glibc.static
  ];

  checkInputs = [ gtest ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DBUILD_TESTING=${if withTests then "ON" else "OFF"}"
    "-DAFTERGLOW_DEPS_ARCHIVES=${depsArchives}"
  ];

  doCheck = withTests;
  # The tests run uninstalled, from the build tree, so nothing wraps them.
  # nixpkgs' Qt finds QML modules through NIXPKGS_QT6_QML_IMPORT_PATH and
  # plugins through QT_PLUGIN_PATH, and wrapQtAppsHook puts both only into the
  # wrapper arguments of installed programs. Export the same entries here.
  # Then the offscreen platform for the GUI tests, and a writable HOME.
  preCheck = ''
    for ((i = 0; i + 3 < ''${#qtWrapperArgs[@]}; i++)); do
      if [[ "''${qtWrapperArgs[i]}" == --prefix ]]; then
        case "''${qtWrapperArgs[i + 1]}" in
          NIXPKGS_QT6_QML_IMPORT_PATH | QT_PLUGIN_PATH)
            addToSearchPath "''${qtWrapperArgs[i + 1]}" "''${qtWrapperArgs[i + 3]}"
            ;;
        esac
      fi
    done
    export NIXPKGS_QT6_QML_IMPORT_PATH QT_PLUGIN_PATH
    export QT_QPA_PLATFORM=offscreen
    export HOME="$(mktemp -d)"
    export CTEST_OUTPUT_ON_FAILURE=1
  '';

  meta = {
    description = "A terminal for Wayland with shader packs, splits, shell integration and Luau scripts";
    homepage = "https://github.com/phosphor-works/Afterglow";
    mainProgram = "afterglow";
    license = with lib.licenses; [
      gpl3Plus
      lgpl21Plus
      zlib
      bsd2
      bsd3
      ijg
      libpng2
      mit
    ];
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
    ];
  };
}
