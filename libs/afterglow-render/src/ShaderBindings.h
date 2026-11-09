// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorShaders/ShaderBindings.h>

namespace AfterglowRender::Bindings {

// Afterglow's bindings above Phosphor's table (PhosphorShaders::Bindings),
// as the contract headers in data/shaders/shared declare them. Every pass
// that runs a pack's program binds them all.

/// uPreviousText in terminal_history.glsl.
inline constexpr int PreviousText = PhosphorShaders::Bindings::kExtraBase;
/// TerminalScene in terminal_scene.glsl: object matrices for mesh passes.
inline constexpr int Scene = PreviousText + 1;
inline constexpr int Panes = Scene + 1;
inline constexpr int TabImage = Panes + 1;
inline constexpr int TabAtlas = TabImage + 1;

} // namespace AfterglowRender::Bindings
