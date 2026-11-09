// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/ShaderProgram.h>

#include <QPointF>
#include <QSize>

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
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

/// The pointer maps of shader packs (pPointerMap), for the mouse: where the
/// content that a pack shows at a canvas position comes from. A pack's map
/// is evaluated on the GPU into a grid of GridColumns columns, read back
/// once, and kept for its canvas size and parameter values. Between grid
/// points the map is bilinear.
///
/// Runs offscreen frames of its own: call it outside a frame (an input
/// event, not the render loop).
class PointerMaps
{
public:
    static constexpr int GridColumns = 128;

    explicit PointerMaps(GpuDevice& device);
    ~PointerMaps();
    PointerMaps(const PointerMaps&) = delete;
    PointerMaps& operator=(const PointerMaps&) = delete;

    /// @p point (pixels of a canvas of @p canvas) through @p program's map,
    /// with @p overrides over its parameter values. A program without a map,
    /// or a map that cannot be evaluated, gives @p point back.
    QPointF unwarp(const ShaderProgramPtr& program, const QSize& canvas, const ShaderParameterValues* overrides,
                   const QPointF& point);
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
        std::uint64_t used = 0;
    };
    const Grid* grid(const ShaderProgramPtr& program, const QSize& canvas, const ShaderParameterValues* overrides);
    bool evaluate(Grid& grid, const TerminalUniforms& uniforms);
    bool ensureResources();
    QRhiGraphicsPipeline* pipeline(const ShaderProgram& program);

    GpuDevice& m_device;
    std::vector<Grid> m_grids;
    std::uint64_t m_clock = 0;
    bool m_failed = false;
    QShader m_vertex;
    std::unique_ptr<QRhiBuffer> m_uniforms;
    std::unique_ptr<QRhiBuffer> m_scene;
    std::unique_ptr<QRhiBuffer> m_panes;
    std::unique_ptr<QRhiTexture> m_dummy;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiShaderResourceBindings> m_bindings;
    std::unique_ptr<QRhiTexture> m_target;
    std::unique_ptr<QRhiTextureRenderTarget> m_renderTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> m_pass;
    int m_targetFormat = 0;
    struct Pipeline
    {
        ShaderProgramPtr program;
        std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    };
    std::unordered_map<const ShaderProgram*, Pipeline> m_pipelines;
};

} // namespace AfterglowRender
