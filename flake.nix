# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later

{
  description = "Afterglow, a terminal for Wayland with shader packs, splits, shell integration and Luau scripts";

  # The same nixpkgs as PlasmaZones' flake.lock, so both build against one Qt.
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: lib.genAttrs supportedSystems (system: f nixpkgs.legacyPackages.${system});

      # project(Afterglow VERSION x.y.z) in CMakeLists.txt is the one source of
      # the version.
      version =
        let
          m = builtins.match ".*project\\(Afterglow VERSION ([0-9.]+).*" (builtins.readFile ./CMakeLists.txt);
        in
        if m == null then throw "no project(Afterglow VERSION ...) in CMakeLists.txt" else builtins.head m;

      package =
        pkgs: withTests:
        pkgs.callPackage ./nix/package.nix {
          inherit version withTests;
          src = self;
        };
    in
    {
      packages = forAllSystems (pkgs: {
        afterglow = package pkgs false;
        default = package pkgs false;
      });

      # `nix flake check` builds Afterglow and the Phosphor libraries it is
      # built with, and runs the test suite.
      checks = forAllSystems (pkgs: {
        tests = package pkgs true;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ (package pkgs true) ];
          packages = [
            pkgs.ccache
            pkgs.clang-tools
          ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-rfc-style);
    };
}
