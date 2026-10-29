// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/ShaderProgram.h>
#include <AfterglowRender/afterglowrender_export.h>

#include <QColor>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>

#include <map>
#include <memory>
#include <vector>

class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiRenderTarget;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;

namespace AfterglowRender {

class GpuDevice;
struct PaneSnapshot;

/// Per-region inputs of terminal shaders, in framebuffer pixels (top-left origin).
struct ShaderRegionState
{
    QRect rect;
    const PaneSnapshot* snapshot = nullptr; ///< colours and palette
    QRectF cursor;
    QRectF previousCursor;
    QColor cursorColor;
    QColor previousCursorColor;
    int cursorStyle = 0; ///< Ghostty CURSORSTYLE_*
    int previousCursorStyle = 0;
    bool cursorVisible = false;
    float cursorChangeTime = 0;
    bool focused = false;
    float focusChangeTime = 0;
    QSizeF cellSize;
    QPointF mouse;
    float backgroundOpacity = 1.0f;
};

struct ShaderFrameTime
{
    float time = 0;
    float delta = 0;
    int frame = 0;
};

/// Post-processing for one window: the grid renders into an offscreen
/// texture, pane shaders run over each pane's region, window shaders over the
/// whole window, and the last pass writes to the swapchain. All on the shared
/// GpuDevice.
class AFTERGLOWRENDER_EXPORT PostChain
{
public:
    explicit PostChain(GpuDevice& device);
    ~PostChain();

    PostChain(const PostChain&) = delete;
    PostChain& operator=(const PostChain&) = delete;

    void setChains(std::vector<ShaderProgramPtr> pane, std::vector<ShaderProgramPtr> window);
    bool active() const noexcept { return !m_pane.empty() || !m_window.empty(); }
    /// Some shader reads iTime: the window renders every frame.
    bool animated() const noexcept;

    /// Makes sure the offscreen textures match @p size and returns the target
    /// the grid pass renders into.
    QRhiTextureRenderTarget* prepare(const QSize& size);
    /// The render pass the grid's target uses (for its pipelines).
    QRhiRenderPassDescriptor* renderPass() const noexcept { return m_gridPass.get(); }

    /// Records the shader passes after the grid pass. Returns with the final
    /// pass on @p swapTarget begun and drawn, so the caller can add the
    /// chrome before ending it.
    void record(QRhiCommandBuffer* cb, const std::vector<ShaderRegionState>& panes, const ShaderRegionState& window,
                const ShaderFrameTime& time, QRhiRenderTarget* swapTarget, QRhiRenderPassDescriptor* swapPass);

    void release();

private:
    struct Pipelines
    {
        std::unique_ptr<QRhiGraphicsPipeline> offscreen;
        std::unique_ptr<QRhiGraphicsPipeline> swapchain;
    };
    QRhiGraphicsPipeline* pipeline(const ShaderProgram& program, QRhiRenderPassDescriptor* pass, bool toSwapchain);
    QRhiShaderResourceBindings* bindings(int ubo, int texture);
    QRhiBuffer* uniformBuffer(int index);
    void fill(TerminalShaderUniforms& u, const ShaderRegionState& s, const ShaderFrameTime& t,
              const ShaderProgram& program) const;

    GpuDevice& m_device;
    std::vector<ShaderProgramPtr> m_pane;
    std::vector<ShaderProgramPtr> m_window;
    std::map<const ShaderProgram*, Pipelines> m_pipelines;
    QShader m_vertex;

    QSize m_size;
    std::unique_ptr<QRhiTexture> m_textures[2];
    // The grid clears its target; shader passes preserve theirs (pane passes
    // only redraw pane regions over a copy of their input).
    std::unique_ptr<QRhiTextureRenderTarget> m_gridTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> m_gridPass;
    std::unique_ptr<QRhiTextureRenderTarget> m_targets[2];
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPass;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::vector<std::unique_ptr<QRhiBuffer>> m_ubos;
    std::map<std::pair<int, int>, std::unique_ptr<QRhiShaderResourceBindings>> m_bindings;
};

} // namespace AfterglowRender
