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

#include <array>
#include <map>
#include <memory>
#include <tuple>
#include <set>
#include <vector>

class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiRenderTarget;
class QRhiResourceUpdateBatch;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;

namespace AfterglowRender {

class RenderFrame;

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
    int cursorStyle = 0; ///< kCursor* in terminal_uniforms.glsl
    int previousCursorStyle = 0;
    bool cursorVisible = false;
    float cursorChangeTime = 0; ///< shader time of the last cursor move
    bool focused = false;
    float focusChangeTime = 0; ///< shader time of the last focus change
    QSizeF cellSize;
    QPointF mouse;
    float backgroundOpacity = 1.0f;
    std::array<float, 3> scrollback{}; ///< pane rows, current-tab rows, eased current-tab rows
    /// Script signals: value and shader time when set.
    std::array<std::array<float, 2>, 8> signals{};
    ShaderParameterMap shaderParameters;
};

struct ShaderFrameTime
{
    float time = 0;
    float delta = 0;
    int frame = 0;
};

/// The shader chains of a window, each run in order.
struct ShaderChains
{
    /// Layer chains: per pane, over that layer alone (premultiplied, the rest
    /// of the layer transparent), before the layers are put together.
    std::vector<ShaderProgramPtr> background; ///< default and cell backgrounds, images under the text
    std::vector<ShaderProgramPtr> text; ///< glyphs, decorations, images over the text
    std::vector<ShaderProgramPtr> cursor;
    /// Per pane, over the whole pane.
    std::vector<ShaderProgramPtr> pane;
    /// Once, over the whole window.
    std::vector<ShaderProgramPtr> window;
    /// The pane overview, after the window chain, only while the overview shows.
    std::vector<ShaderProgramPtr> overview;

    bool readsScrollback() const noexcept
    {
        for (const auto* chain : {&background, &text, &cursor, &pane, &window, &overview}) {
            for (const auto& program : *chain) {
                if (program->readsScrollback()) return true;
            }
        }
        return false;
    }

    bool audioReactive() const noexcept
    {
        for (const auto* chain : {&background, &text, &cursor, &pane, &window, &overview}) {
            for (const auto& program : *chain) {
                if (program->requiresAudio()) return true;
            }
        }
        return false;
    }

    bool highDynamicRange() const noexcept
    {
        for (const auto* chain : {&background, &text, &cursor, &pane, &window, &overview}) {
            for (const auto& program : *chain) {
                if (program->requiresHdr()) {
                    return true;
                }
            }
        }
        return false;
    }

    bool textHistory() const noexcept
    {
        for (const auto* chain : {&background, &text, &cursor, &pane, &window, &overview}) {
            for (const auto& program : *chain) {
                if (program->readsPreviousText()) {
                    return true;
                }
            }
        }
        return false;
    }
    bool layered() const noexcept { return !background.empty() || !text.empty() || !cursor.empty() || textHistory(); }
    bool empty() const noexcept { return !layered() && pane.empty() && window.empty(); }
};

/// Post-processing for one window: the grid renders into an offscreen
/// texture, pane shaders run over each pane's region, window shaders over the
/// whole window, and the last pass writes to the swapchain. All on the shared
/// GpuDevice.
///
/// With layer chains the grid instead renders each layer into its own
/// texture; their chains run, and the layers are put together into the grid
/// texture before the pane and window chains.
class AFTERGLOWRENDER_EXPORT PostChain
{
public:
    enum class Layer { Background, Text, Cursor, OtherCursor };
    static constexpr int LayerCount = 4;

    explicit PostChain(GpuDevice& device);
    ~PostChain();

    PostChain(const PostChain&) = delete;
    PostChain& operator=(const PostChain&) = delete;

    void setChains(ShaderChains chains);
    bool audioReactive() const noexcept { return m_chains.audioReactive(); }
    bool readsScrollback() const noexcept { return m_chains.readsScrollback(); }
    bool active() const noexcept { return !m_chains.empty() || (m_overviewShowing && !m_chains.overview.empty()); }
    /// Runs the overview chain after the window chain while the overview shows.
    void setOverviewShowing(bool showing) { m_overviewShowing = showing; }
    /// The pane block for this frame (terminal_panes.glsl), into @p updates
    /// before the frame's first pass.
    void uploadPanes(QRhiResourceUpdateBatch* updates, const TerminalPaneUniforms& panes);
    bool layered() const noexcept { return m_chains.layered(); }
    /// Some shader reads iTime: the window renders every frame.
    bool animated() const noexcept;

    /// Makes sure the offscreen textures match @p size and returns the target
    /// the grid pass renders into.
    QRhiTextureRenderTarget* prepare(const QSize& size);
    /// The render pass the grid's target uses (for its pipelines). Layer
    /// targets use a compatible one.
    QRhiRenderPassDescriptor* renderPass() const noexcept { return m_gridPass.get(); }

    /// After prepare(), with layer chains: the target @p layer renders into,
    /// cleared to transparent.
    QRhiTextureRenderTarget* layerTarget(Layer layer);
    /// Runs the layer chains, then begins a pass on the grid target and
    /// composites the layers into it: @p clear outside the panes, and inside
    /// each pane background, cursor and text, with the cursor over the text
    /// unless @p cursorBelowText says otherwise for that pane. Returns with
    /// the pass open, so the caller can draw overlays before ending it.
    void composite(RenderFrame& frame, const std::vector<ShaderRegionState>& panes,
                   const std::vector<bool>& cursorBelowText, const ShaderFrameTime& time, const QColor& clear, bool otherCursors = false);

    /// Records the shader passes after the grid pass. Returns with the final
    /// pass on @p swapTarget begun and drawn, so the caller can add the
    /// chrome before ending it.
    void record(RenderFrame& frame, const std::vector<ShaderRegionState>& panes, const ShaderRegionState& window,
                const ShaderFrameTime& time, QRhiRenderTarget* swapTarget, QRhiRenderPassDescriptor* swapPass);

    /// After the final pass ends, retain the processed text for uPreviousText.
    /// Allocated and copied only when a shader reads that sampler.
    void finishFrame(QRhiCommandBuffer* cb);

    void release();

private:
    QRhiGraphicsPipeline* pipeline(const ShaderProgram& program, QRhiRenderPassDescriptor* pass);
    struct BufferInputs {
        QRhiTexture* depth = nullptr;
        std::array<QRhiTexture*, MaxShaderBuffers> textures{};
        std::array<QRhiSampler*, MaxShaderBuffers> samplers{};
        std::array<QSize, MaxShaderBuffers> sizes{};
    };
    QRhiShaderResourceBindings* bindings(int ubo, int texture, const ShaderRegionState* region = nullptr,
        const ShaderProgram* program = nullptr, const BufferInputs* buffers = nullptr, QRhiResourceUpdateBatch* updates = nullptr);
    QRhiTexture* localTexture(QRhiTexture* source, const QRect& rect, QRhiResourceUpdateBatch* updates);
    QRhiTexture* parameterTexture(const QImage& image, QRhiResourceUpdateBatch* updates);
    QRhiShaderResourceBindings* compositeBindings(int ubo, int background, int text, int cursor, int other);
    QRhiGraphicsPipeline* compositePipeline();
    /// Runs @p chain over each pane region of the layer in texture @p input,
    /// writing to free layer textures; returns the texture with the result.
    int runLayerChain(RenderFrame& frame, const std::vector<ShaderProgramPtr>& chain, int input,
                      const std::vector<ShaderRegionState>& panes, const ShaderFrameTime& time, int& ubo, int scope);
    QRhiBuffer* uniformBuffer(int index);
    /// The scene block (terminal_scene.glsl) that goes with uniform buffer @p index.
    QRhiBuffer* sceneBuffer(int index);
    QRhiBuffer* panesBuffer();
    void fill(TerminalUniforms& u, const ShaderRegionState& s, const ShaderFrameTime& t,
              const ShaderProgram& program, const BufferInputs* buffers = nullptr) const;

    struct BufferState;
    std::vector<BufferInputs> runBuffers(RenderFrame& frame, const ShaderProgram& program, int scope, int step,
                           int input, const std::vector<ShaderRegionState>& regions,
                           const ShaderFrameTime& time, int& ubo);
    using BufferKey = std::tuple<const ShaderProgram*, int, int, const PaneSnapshot*>;
    std::map<BufferKey, std::array<std::unique_ptr<BufferState>, MaxShaderBuffers>> m_buffers;
    std::set<std::tuple<const ShaderProgram*, int, int>> m_bufferInstances;
    struct CachedTexture { std::unique_ptr<QRhiTexture> texture; std::uint64_t used = 0; std::uint64_t bytes = 0; };
    std::map<std::tuple<QRhiTexture*, int, int, int, int>, CachedTexture> m_localTextures;
    std::map<qint64, CachedTexture> m_parameterTextures;
    std::unique_ptr<QRhiTexture> m_emptyTexture;
    bool m_inputBudgetWarned = false;
    std::uint64_t m_frame = 0;
    std::uint64_t m_bufferBytes = 0;
    static constexpr std::uint64_t BufferBudget = 256 * 1024 * 1024;
    static constexpr int MinimumBufferSide = 16; // a pass that would shrink below this is transparent instead
    GpuDevice& m_device;
    ShaderChains m_chains;
    bool m_overviewShowing = false;
    std::unique_ptr<QRhiBuffer> m_panesUbo;
    std::map<std::pair<const ShaderProgram*, const QRhiRenderPassDescriptor*>, std::unique_ptr<QRhiGraphicsPipeline>>
        m_pipelines;
    QShader m_vertex;
    QShader m_compositeShader;
    std::unique_ptr<QRhiGraphicsPipeline> m_composite;

    QSize m_size;
    // 0 and 1: the ping-pong pair of the pane and window chains. From
    // FirstLayerTexture: one per layer and a spare, for the layer chains.
    static constexpr int FirstLayerTexture = 2;
    static constexpr int TextureCount = FirstLayerTexture + LayerCount + 1;
    std::unique_ptr<QRhiTexture> m_textures[TextureCount];
    std::unique_ptr<QRhiTextureRenderTarget> m_layerTargets[LayerCount + 1]; ///< clearing, by texture - FirstLayerTexture
    int m_layerTexture[LayerCount] = {}; ///< which texture holds each layer this frame
    // The grid clears its target; shader passes preserve theirs (pane passes
    // only redraw pane regions over a copy of their input).
    std::unique_ptr<QRhiTextureRenderTarget> m_gridTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> m_gridPass;
    std::unique_ptr<QRhiTextureRenderTarget> m_targets[2];
    std::unique_ptr<QRhiTextureRenderTarget> m_windowTargets[2]; ///< clearing, for deformed quads
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPass;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiTexture> m_previousText;
    std::unique_ptr<QRhiTextureRenderTarget> m_historyTarget;
    using HistoryRegion = std::tuple<const PaneSnapshot*, QRect, std::uint32_t, std::uint32_t, QSizeF, float>;
    std::vector<HistoryRegion> m_historyRegions;
    bool m_historyValid = false;
    float m_historyTime = 0;
    std::vector<std::unique_ptr<QRhiBuffer>> m_ubos;
    std::vector<std::unique_ptr<QRhiBuffer>> m_sceneUbos;
    std::vector<std::unique_ptr<QRhiShaderResourceBindings>> m_bindings;
    std::map<std::array<int, 5>, std::unique_ptr<QRhiShaderResourceBindings>> m_compositeBindings;
    std::vector<std::unique_ptr<QRhiBuffer>> m_compositeUbos;
    /// Uniform buffers used so far this frame: a dynamic buffer holds one
    /// value per frame, so every pass takes its own.
    int m_nextUbo = 0;
};

} // namespace AfterglowRender
