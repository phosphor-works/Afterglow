// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorShaders/ShaderBindings.h>

namespace AfterglowRender::Bindings {

// Afterglow's bindings above Phosphor's table (PhosphorShaders::Bindings),
// as the contract headers in data/shaders/shared declare them. Every pass
// that runs a pack's program binds them all. A pointer map binds what its
// stage declares (PointerMaps::program).

/// uPreviousText in terminal_history.glsl.
inline constexpr int PreviousText = PhosphorShaders::Bindings::kExtraBase;
/// TerminalScene in terminal_scene.glsl: object matrices for mesh passes.
inline constexpr int Scene = PreviousText + 1;
inline constexpr int Panes = Scene + 1;
inline constexpr int TabImage = Panes + 1;
inline constexpr int TabAtlas = TabImage + 1;

// The headers write these numbers out: a change to Phosphor's table must
// not move them away from the GLSL.
static_assert(PreviousText == 17 && Scene == 18 && Panes == 19 && TabImage == 20 && TabAtlas == 21,
              "the bindings of terminal_history.glsl, terminal_scene.glsl and terminal_panes.glsl");

} // namespace AfterglowRender::Bindings
