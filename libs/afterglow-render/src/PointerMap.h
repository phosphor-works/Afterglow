// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/ShaderProgram.h>

#include <QPointF>
#include <QRect>
#include <QSize>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;

namespace AfterglowRender {

class GpuDevice;
class RenderFrame;

/// The pointer maps of shader packs (pPointerMap), for the mouse: where the
/// content that a pack shows at a canvas position comes from. A pack's map
/// is evaluated on the GPU into a grid with a point every Spacing pixels of
/// the canvas, in the frame
/// that first draws the pack with its canvas size and parameter values, and
/// read back without a wait: its grid takes effect frames later, when the
/// readback completes. Between grid points the map is bilinear.
///
/// Lookups use the maps of the regions as the last frames drew them, so a
/// lookup never touches the GPU: it can run at any time, also inside a
/// frame or before the device exists.
class PointerMaps
{
public:
    /// Canvas pixels between grid points: fine enough for a perspective
    /// near its horizon, where a map bends most.
    static constexpr int Spacing = 8;
    /// The points of a grid along each side, within these limits.
    static constexpr int MinPoints = 8;
    static constexpr int MaxPoints = 256;

    explicit PointerMaps(GpuDevice& device);
    ~PointerMaps();
    PointerMaps(const PointerMaps&) = delete;
    PointerMaps& operator=(const PointerMaps&) = delete;

    /// A region that a frame draws: the window (no pane), or a pane by its
    /// PaneParams::id, at @p rect (device pixels), through the mapped packs
    /// of @p chain with the live @p parameters. Asks for the grids that it
    /// needs; record() evaluates them.
    void show(std::optional<std::uint32_t> pane, const QRect& rect, const std::vector<ShaderProgramPtr>& chain,
              const ShaderParameterMap& parameters);
    /// Ends the regions of a frame: panes that it did not draw forget theirs.
    void endRegions();
    /// Takes the grids whose readbacks completed. Call it in a frame before
    /// show(): a readback completes when a later frame begins.
    void collect();
    /// Evaluates the grids that show() asked for, outside a pass of @p frame.
    void record(RenderFrame& frame);
    /// Grids are evaluated or read back: frames must come until they arrive.
    bool pending() const;
    /// Changes when a region's maps change, so the pointer can be looked at again.
    std::uint64_t serial() const { return m_serial; }

    /// @p point (device pixels of the window) through the maps of the
    /// window, or of the pane @p pane, last pack first. A region with no
    /// map, or one not drawn yet, gives @p point back.
    QPointF unwarp(std::optional<std::uint32_t> pane, const QPointF& point) const;

    /// Forgets the grids and pipelines: the chains changed.
    void clear();

private:
    struct Grid
    {
        ShaderProgramPtr program; ///< kept alive: the key compares its address
        QSize canvas;
        std::array<float, 32> params{};
        std::array<float, 64> colors{};
        int columns = 0;
        int rows = 0;
        std::vector<float> values; ///< x, y of each grid point, row by row
        bool queued = false; ///< waits for record()
        bool ready = false;
        int attempts = 0; ///< readbacks that came back not this frame's
        std::uint64_t used = 0;
        QPointF unwarp(const QPointF& point) const;
    };
    using GridPtr = std::shared_ptr<Grid>;
    struct Region
    {
        QRect rect; ///< of the grids in @c shown
        std::vector<GridPtr> shown; ///< ready, in chain order
        QRect wantedRect;
        std::vector<GridPtr> wanted; ///< the last frame's, until all are ready
        std::uint64_t seenFrame = 0;
    };
    /// A readback in flight: Qt holds a pointer to @c result until it
    /// completes, so it lives on the heap and outlives a failed finish().
    struct Readback;
    struct Program
    {
        std::unique_ptr<QRhiGraphicsPipeline> pipeline;
        std::vector<std::unique_ptr<QRhiShaderResourceBindings>> bindings; ///< one per uniform slot
    };

    GridPtr grid(const ShaderProgramPtr& program, const QSize& canvas, const ShaderParameterValues* overrides);
    void settle(Region& region);
    /// Marks @p program's map off, once, and lets the regions that waited
    /// for its grids show the rest.
    void failProgram(const ShaderProgram* program, const char* why);
    bool ensureResources();
    bool ensureTarget(int rows);
    Program* program(const ShaderProgramPtr& program);

    GpuDevice& m_device;
    std::vector<GridPtr> m_grids;
    std::optional<Region> m_window;
    std::unordered_map<std::uint32_t, Region> m_panes;
    std::vector<std::unique_ptr<Readback>> m_readbacks;
    std::unordered_set<const ShaderProgram*> m_failedPrograms; ///< logged once, until clear()
    std::uint64_t m_clock = 0;
    std::uint64_t m_frame = 0;
    std::uint64_t m_serial = 0;
    int m_canary = 0; ///< the last readback's mark (Readback::canary)
    bool m_failed = false;
    QShader m_vertex;
    int m_uniformStride = 0;
    std::unique_ptr<QRhiBuffer> m_uniforms;
    std::unique_ptr<QRhiBuffer> m_zeros; ///< the contract's other blocks, read as zeros
    int m_zerosSize = 0;
    std::unique_ptr<QRhiTexture> m_dummy; ///< one transparent pixel for every sampler
    std::unique_ptr<QRhiSampler> m_sampler;
    bool m_uploaded = false;
    std::unique_ptr<QRhiRenderPassDescriptor> m_pass; ///< before the targets that use it
    std::unique_ptr<QRhiTexture> m_target;
    std::unique_ptr<QRhiTextureRenderTarget> m_renderTarget;
    std::unordered_map<const ShaderProgram*, std::pair<ShaderProgramPtr, Program>> m_programs;
};

} // namespace AfterglowRender
