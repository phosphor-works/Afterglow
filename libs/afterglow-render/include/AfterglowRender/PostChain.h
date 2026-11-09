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

#include <algorithm>
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
class QRhiShaderResourceBinding;
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
    double cursorChangeTime = -1e6; ///< shader time of the last cursor move; -1e6 never
    bool focused = false;
    double focusChangeTime = -1e6; ///< shader time of the last focus change; -1e6 never
    QSizeF cellSize;
    QPointF mouse;
    float backgroundOpacity = 1.0f;
    std::array<float, 3> scrollback{}; ///< pane rows, current-tab rows, eased current-tab rows
    /// Script signals: value, and the shader time when set (-1e6 never).
    struct Signal
    {
        float value = 0;
        double at = -1e6;
    };
    std::array<Signal, 8> signals{};
    ShaderParameterMap shaderParameters;
    /// In an overlay transition pass, how far the overlay has faded in: 0 the
    /// pane under it, 1 the overlay (uOverlayTransition). Negative elsewhere.
    float overlayTransition = -1.0f;
    /// The pane under a fading overlay (captureOverlayHosts): it draws before
    /// the frame's own panes, with a text history of its own. It reads the
    /// window's shared background buffers as the last frame left them,
    /// unless they do not exist yet: then it makes and steps them, and the
    /// frame's own panes take that result.
    bool overlayHost = false;
    /// A popup over the pane, in framebuffer pixels: the text there is the
    /// popup's. Empty without one, and for the pane under a fading popup.
    QRect popup;
};

/// Seconds are doubles until they are written to the uniforms: a float of
/// seconds since the window opened steps by milliseconds after a day.
struct ShaderFrameTime
{
    double time = 0;
    float delta = 0;
    int frame = 0;
    std::array<float, 4> date{}; ///< iDate: year, month, day, seconds since midnight
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
    /// A tab switch, after the window chain, only while it runs.
    std::vector<ShaderProgramPtr> tabTransition;
    /// Panes opening and closing, after the window chain, only while they do.
    std::vector<ShaderProgramPtr> paneEffects;
    /// An overlay fading in or out over a pane: over that pane, after the pane
    /// chain and before the window chain, only while it fades.
    std::vector<ShaderProgramPtr> overlayTransition;

    /// Some program of any chain, event chains included, matches @p test.
    template<typename Test>
    bool any(Test test) const
    {
        for (const auto* chain : {&background, &text, &cursor, &pane, &window, &overview, &tabTransition, &paneEffects,
                                  &overlayTransition}) {
            if (anyIn(*chain, test)) return true;
        }
        return false;
    }
    /// Some program of an always-on chain (layers, panes, window) matches @p test.
    template<typename Test>
    bool anyAlwaysOn(Test test) const
    {
        for (const auto* chain : {&background, &text, &cursor, &pane, &window}) {
            if (anyIn(*chain, test)) return true;
        }
        return false;
    }
    template<typename Test>
    static bool anyIn(const std::vector<ShaderProgramPtr>& chain, Test test)
    {
        return std::any_of(chain.begin(), chain.end(), [&](const ShaderProgramPtr& program) { return test(*program); });
    }

    bool readsScrollback() const { return any([](const ShaderProgram& p) { return p.readsScrollback(); }); }
    /// Some chain reads the pointer: the window renders when it moves.
    bool readsMouse() const { return any([](const ShaderProgram& p) { return p.readsMouse(); }); }
    /// Whether audio capture may be needed at all. PostChain::audioReactive()
    /// counts an event chain only while it shows.
    bool audioReactive() const { return any([](const ShaderProgram& p) { return p.requiresAudio(); }); }
    /// No always-on chain: post-processing runs only while an event chain shows.
    bool alwaysOnEmpty() const noexcept
    {
        return background.empty() && text.empty() && cursor.empty() && pane.empty() && window.empty();
    }
    bool empty() const noexcept
    {
        return alwaysOnEmpty() && overview.empty() && tabTransition.empty() && paneEffects.empty() && overlayTransition.empty();
    }
    /// The programs of every chain, event chains included.
    std::size_t size() const noexcept
    {
        std::size_t count = 0;
        for (const auto* chain : {&background, &text, &cursor, &pane, &window, &overview, &tabTransition, &paneEffects,
                                  &overlayTransition}) {
            count += chain->size();
        }
        return count;
    }
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
    const ShaderChains& chains() const noexcept { return m_chains; }
    /// Audio capture feeds a chain that runs this frame.
    bool audioReactive() const;
    bool readsScrollback() const { return m_chains.readsScrollback(); }
    bool active() const noexcept { return !m_chains.alwaysOnEmpty() || eventChain() != nullptr || overlayTransitionShowing(); }
    /// An overlay fades in or out this frame, and the overlay transition chain
    /// is there to draw it.
    void setOverlayTransitionShowing(bool showing) { m_overlayTransitionShowing = showing; }
    /// Shaders.overlayTransition compiled: overlays fade through it.
    bool hasOverlayTransition() const noexcept { return !m_chains.overlayTransition.empty(); }
    bool overlayTransitionShowing() const noexcept { return m_overlayTransitionShowing && !m_chains.overlayTransition.empty(); }
    /// After the grid target holds the panes under the fading overlays at
    /// their places (with layer chains, after composite() of them): runs the
    /// pane chain over @p hosts, and keeps the result in the tab image
    /// (uTabImage), where the overlay transition reads it. Then the grid
    /// target is free for the frame's own grid.
    void captureOverlayHosts(RenderFrame& frame, const std::vector<ShaderRegionState>& hosts, const ShaderFrameTime& time);
    /// The regions that the overlay transition chain runs over in the next
    /// record(): the panes with their overlays, each with its progress in
    /// ShaderRegionState::overlayTransition.
    void setOverlayTransitions(std::vector<ShaderRegionState> regions) { m_overlayTransitions = std::move(regions); }
    /// Runs the pane effects chain after the window chain while panes open,
    /// close or are away; under the overview or a tab switch it runs first.
    void setPaneEffectsShowing(bool showing) { m_paneEffectsShowing = showing; }
    /// Runs the overview chain after the window chain while the overview shows.
    void setOverviewShowing(bool showing) { m_overviewShowing = showing; }
    /// Runs the tab transition chain after the window chain while a switch runs.
    void setTransitionShowing(bool showing) { m_transitionShowing = showing; }
    /// The pane block for this frame (terminal_panes.glsl), into @p updates
    /// before the frame's first pass.
    void uploadPanes(QRhiResourceUpdateBatch* updates, const TerminalPaneUniforms& panes);
    /// After prepare(): a clearing, window-sized target for a tab that is not
    /// the current one (an overview card, the tab a switch leaves, or a
    /// closing pane), compatible with the grid's pipelines. All of them share
    /// it in turn. The panes under fading overlays (captureOverlayHosts) use
    /// it in frames where none of those does. terminal_panes.glsl samples it
    /// as uTabImage. It is freed after a frame that did not ask for it.
    QRhiTextureRenderTarget* tabImageTarget();
    /// Shrinks the tab in tabImageTarget() into @p tile of the overview
    /// atlas (uTabAtlas), in window pixels.
    void shrinkTabToAtlas(RenderFrame& frame, const QRect& tile);
    /// Layer chains run, or a chain that runs this frame reads the previous text.
    bool layered() const;
    /// An always-on chain animates (time, history, feedback or spin), or an
    /// age-only program's youngest age in the last recorded frame is below
    /// its settle time, so the window renders every frame.
    /// WindowRenderer::animating() covers the event chains. The overlay
    /// transition runs only while its host animates the fade.
    bool animated() const noexcept;

    /// Makes sure the offscreen textures match @p size and returns the target
    /// the grid pass renders into.
    QRhiTextureRenderTarget* prepare(const QSize& size);
    /// The render pass the grid's target uses (for its pipelines). Layer
    /// targets use a compatible one.
    QRhiRenderPassDescriptor* renderPass() const noexcept { return m_gridPass.get(); }
    /// Changes whenever renderPass() is made again. A new descriptor can reuse
    /// the old address, so pipelines made for it compare this instead.
    std::uint64_t renderPassGeneration() const noexcept { return m_passGeneration; }
    /// Frees the window-sized textures, buffers and copies while no chain runs.
    /// Render-pass descriptors and pipelines stay, so the next event starts
    /// without making them again.
    void releaseIdle();
    /// Drops pipelines made for @p pass, which is about to be destroyed.
    void forgetPass(const QRhiRenderPassDescriptor* pass);

    /// After prepare(), with layer chains: the target @p layer renders into,
    /// cleared to transparent.
    QRhiTextureRenderTarget* layerTarget(Layer layer);
    /// Runs the layer chains, then begins a pass on the grid target and
    /// composites the layers into it: @p clear outside the panes, and inside
    /// each pane background, cursor and text, with the cursor over the text
    /// unless @p cursorBelowText says otherwise for that pane. Returns with
    /// the pass open, so the caller can draw overlays before ending it.
    /// @p hosts: the panes under fading overlays (captureOverlayHosts), which
    /// leave the text history alone and take the composite buffers after
    /// @p uboBase, so that the frame's own composite keeps its values.
    void composite(RenderFrame& frame, const std::vector<ShaderRegionState>& panes,
                   const std::vector<bool>& cursorBelowText, const ShaderFrameTime& time, const QColor& clear, bool otherCursors = false,
                   bool hosts = false, std::size_t uboBase = 0);

    /// Records the shader passes after the grid pass: the pane chain, the
    /// overlay transition over setOverlayTransitions(), then the window and
    /// event chains. Returns with the final pass on @p swapTarget begun and
    /// drawn, so the caller can add the docks and the chrome before ending
    /// it. Window passes clear to @p clear (premultiplied).
    void record(RenderFrame& frame, const std::vector<ShaderRegionState>& panes, const ShaderRegionState& window,
                const ShaderFrameTime& time, QRhiRenderTarget* swapTarget, QRhiRenderPassDescriptor* swapPass,
                const QColor& clear);

    /// After the final pass ends, retain the processed text for uPreviousText.
    /// Allocated and copied only when a shader reads that sampler.
    void finishFrame(QRhiCommandBuffer* cb);

    void release();

private:
    /// The event chain that runs this frame, if any: the overview, else a tab
    /// switch, else pane effects. The overlay transition is apart: it runs
    /// with any of them (overlayTransitionShowing()).
    const std::vector<ShaderProgramPtr>* eventChain() const noexcept;
    /// Some program that runs this frame (always-on, the showing event chain
    /// or the overlay transition) matches @p test.
    template<typename Test>
    bool anyRunning(Test test) const
    {
        if (m_chains.anyAlwaysOn(test)) return true;
        for (const auto* chain : {&m_chains.overview, &m_chains.tabTransition, &m_chains.paneEffects})
            if (showing(*chain) && ShaderChains::anyIn(*chain, test)) return true;
        return overlayTransitionShowing() && ShaderChains::anyIn(m_chains.overlayTransition, test);
    }
    bool showing(const std::vector<ShaderProgramPtr>& chain) const noexcept;
    bool highDynamicRange() const;
    bool textHistory() const;
    /// The window-sized textures and the shader buffers.
    void releaseTextures();
    /// The window-sized textures only: shader buffers have their own formats
    /// and render passes, so they and their contents stay.
    void releaseTargets();
    void releaseTabImages();
    /// One render-pass descriptor per attachment layout, shared by every
    /// buffer state with that layout, so their pipelines are shared too.
    QRhiRenderPassDescriptor* bufferPass(QRhiTextureRenderTarget* target, int format, int samples, bool depthOutput, bool hardwareDepth);
    QRhiGraphicsPipeline* pipeline(const ShaderProgram& program, QRhiRenderPassDescriptor* pass);
    struct BufferInputs {
        QRhiTexture* depth = nullptr;
        std::array<QRhiTexture*, MaxShaderBuffers> textures{};
        std::array<QRhiSampler*, MaxShaderBuffers> samplers{};
        std::array<QSize, MaxShaderBuffers> sizes{};
    };
    QRhiShaderResourceBindings* bindings(int ubo, int texture, const ShaderRegionState* region = nullptr,
        const ShaderProgram* program = nullptr, const BufferInputs* buffers = nullptr, QRhiResourceUpdateBatch* updates = nullptr,
        QRhiTexture* input = nullptr);
    QRhiTexture* localTexture(QRhiTexture* source, const QRect& rect, QRhiResourceUpdateBatch* updates);
    /// Runs @p chain over @p regions of texture @p current, as the pane chain
    /// runs, and returns the texture with the result. @p scope keys buffers.
    int runRegionChain(RenderFrame& frame, const std::vector<ShaderProgramPtr>& chain, int current,
                       const std::vector<ShaderRegionState>& regions, const ShaderFrameTime& time, int& ubo, int scope);
    QRhiTexture* parameterTexture(const QImage& image, QRhiResourceUpdateBatch* updates);
    /// Whether @p bytes more of copies and images fit the input budget, after
    /// dropping copies that this frame has not used.
    bool fitsInputBudget(std::uint64_t bytes);
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
    // Declared before m_buffers: the buffer targets use these descriptors.
    std::map<std::tuple<int, int, bool, bool>, std::unique_ptr<QRhiRenderPassDescriptor>> m_bufferPasses;
    /// Imported mesh vertices, once per buffer pass program however many panes draw it.
    struct MeshVertices { std::unique_ptr<QRhiBuffer> buffer; bool uploaded = false; };
    std::map<const ShaderProgram*, MeshVertices> m_meshVertices;
    using BufferKey = std::tuple<const ShaderProgram*, int, int, std::uint64_t>;
    std::map<BufferKey, std::array<std::unique_ptr<BufferState>, MaxShaderBuffers>> m_buffers;
    std::set<std::tuple<const ShaderProgram*, int, int>> m_bufferInstances;
    struct CachedTexture { std::unique_ptr<QRhiTexture> texture; std::uint64_t used = 0; std::uint64_t bytes = 0; };
    std::map<std::tuple<QRhiTexture*, int, int, int, int>, CachedTexture> m_localTextures;
    std::map<qint64, CachedTexture> m_parameterTextures;
    std::unique_ptr<QRhiTexture> m_emptyTexture;
    bool m_regionBudgetWarned = false;
    bool m_imageBudgetWarned = false;
    std::uint64_t m_frame = 0;
    std::uint64_t m_bufferBytes = 0;
    static constexpr std::uint64_t BufferBudget = 256 * 1024 * 1024;
    static constexpr int MinimumBufferSide = 16; // a pass that would shrink below this is transparent instead
    GpuDevice& m_device;
    ShaderChains m_chains;
    bool m_overviewShowing = false;
    bool m_transitionShowing = false;
    bool m_paneEffectsShowing = false;
    bool m_overlayTransitionShowing = false;
    std::vector<ShaderRegionState> m_overlayTransitions; ///< for the next record()
    bool m_tabImageUsed = false; ///< tabImageTarget() was called since the last prepare()
    std::unique_ptr<QRhiBuffer> m_panesUbo;
    std::unique_ptr<QRhiTexture> m_tabImage;
    std::unique_ptr<QRhiTexture> m_tabAtlas;
    std::unique_ptr<QRhiTextureRenderTarget> m_tabImageTarget;
    std::unique_ptr<QRhiTextureRenderTarget> m_tabAtlasTarget;
    std::unique_ptr<QRhiTextureRenderTarget> m_tabAtlasClearTarget; ///< the same texture, cleared first
    bool m_tabAtlasCleared = false; ///< this frame's first tile cleared the atlas
    std::map<std::pair<const ShaderProgram*, const QRhiRenderPassDescriptor*>, std::unique_ptr<QRhiGraphicsPipeline>>
        m_pipelines;
    QShader m_vertex;
    QShader m_compositeShader;
    std::unique_ptr<QRhiGraphicsPipeline> m_composite;

    QSize m_size;
    int m_format = -1; ///< QRhiTexture::Format of the textures and descriptors
    std::uint64_t m_passGeneration = 0;
    // 0 and 1: the ping-pong pair of the pane and window chains. From
    // FirstLayerTexture: one per layer and a spare, for the layer chains.
    static constexpr int FirstLayerTexture = 2;
    static constexpr int TextureCount = FirstLayerTexture + LayerCount + 1;
    std::unique_ptr<QRhiTexture> m_textures[TextureCount];
    std::unique_ptr<QRhiTextureRenderTarget> m_layerTargets[LayerCount + 1]; ///< clearing, by texture - FirstLayerTexture
    int m_layerTexture[LayerCount] = {}; ///< which texture holds each layer this frame
    // The grid, layer and window targets clear; pane passes preserve theirs
    // (they only redraw pane regions over a copy of their input).
    std::unique_ptr<QRhiTextureRenderTarget> m_gridTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> m_gridPass;
    std::unique_ptr<QRhiTextureRenderTarget> m_targets[2];
    std::unique_ptr<QRhiTextureRenderTarget> m_windowTargets[2]; ///< clearing, for deformed quads
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPass;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiTexture> m_previousText;
    std::unique_ptr<QRhiTextureRenderTarget> m_historyTarget;
    /// A region's identity: PaneSnapshot::identity (0 for a window-wide
    /// region), rectangle, default colours, cell size, opacity, and the
    /// popup over it, whose text its history holds there.
    using HistoryRegion = std::tuple<std::uint64_t, QRect, std::uint32_t, std::uint32_t, QSizeF, float, QRect>;
    std::vector<HistoryRegion> m_historyRegions;
    bool m_historyValid = false;
    double m_historyTime = 0;
    static std::vector<HistoryRegion> historyRegions(const std::vector<ShaderRegionState>& panes);
    /// Copies into @p to, from @p from, each of @p regions that @p held
    /// (what @p from holds) has too, at its place.
    void copyHistoryRegions(RenderFrame& frame, QRhiTexture* to, QRhiTexture* from,
                            const std::vector<HistoryRegion>& regions, const std::vector<HistoryRegion>& held);
    /// A chain that runs over the panes under fading overlays (the layer and
    /// pane chains) reads the previous text: they keep a host history.
    bool hostsReadHistory() const;
    /// The text history of the panes under fading overlays: the frame's own
    /// history holds the overlays' text then. It starts from that history
    /// where it held the same panes. In the frame after the last fade, the
    /// frame's own history takes the panes' text back from it, and then it
    /// goes.
    std::unique_ptr<QRhiTexture> m_hostHistory;
    std::unique_ptr<QRhiTextureRenderTarget> m_hostHistoryTarget;
    std::vector<HistoryRegion> m_hostHistoryRegions;
    double m_hostHistoryTime = 0;
    std::uint64_t m_hostHistoryFrame = 0; ///< m_frame of the last host composite
    /// The newest event of each EventAge kind among the last recorded frame's
    /// regions, in shader time, and that frame's time: what animated() ages.
    std::array<double, EventAgeKinds> m_eventTimes{-1e6, -1e6, -1e6};
    double m_eventClock = 0;
    std::vector<std::unique_ptr<QRhiBuffer>> m_ubos;
    std::vector<std::unique_ptr<QRhiBuffer>> m_sceneUbos;
    std::vector<std::unique_ptr<QRhiShaderResourceBindings>> m_bindings;
    std::vector<QRhiShaderResourceBinding> m_bindingEntries; ///< reused by bindings()
    std::map<std::array<int, 5>, std::unique_ptr<QRhiShaderResourceBindings>> m_compositeBindings;
    std::vector<std::unique_ptr<QRhiBuffer>> m_compositeUbos;
    /// Uniform buffers used so far this frame: a dynamic buffer holds one
    /// value per frame, so every pass takes its own.
    int m_nextUbo = 0;
};

} // namespace AfterglowRender
