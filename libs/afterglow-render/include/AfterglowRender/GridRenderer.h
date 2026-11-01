// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/PostChain.h>
#include <AfterglowRender/ShaderProgram.h>
#include <AfterglowRender/afterglowrender_export.h>

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QColor>
#include <QSize>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QRhiCommandBuffer;
class QRhiRenderTarget;
class QRhiResourceUpdateBatch;
class QRhiSwapChain;
class QWindow;
typedef struct hb_buffer_t hb_buffer_t;

namespace AfterglowVt {
struct ImageFrame;
using FramePtr = std::shared_ptr<const ImageFrame>;
}

namespace AfterglowRender {

class FontSet;
class GlyphCache;
class GpuDevice;
struct FontResources;
struct PaneSnapshot;

/// GPU instance record shared by every quad the grid draws.
struct Instance
{
    float x, y, w, h;
    float u0, v0, u1, v1;
    std::uint32_t color;
    std::uint32_t kind;
    std::uint32_t atlasPage = 0; ///< CPU draw batching; not a shader attribute
};

enum class InstanceKind : std::uint32_t {
    Solid = 0,
    GrayGlyph = 1,
    ColorGlyph = 2,
    Undercurl = 3,
    Dotted = 4,
    Dashed = 5,
    SubpixelGlyph = 6,
    /// Gray atlas coverage drawn as is, without the text gamma: shade
    /// characters are fixed densities, not antialiased edges.
    LinearGlyph = 7,
};

/// Turns pane snapshots into instances: background runs, shaped text,
/// decorations and the cursor.
class AFTERGLOWRENDER_EXPORT FrameBuilder
{
public:
    FrameBuilder();
    ~FrameBuilder();
    FrameBuilder(const FrameBuilder&) = delete;
    FrameBuilder& operator=(const FrameBuilder&) = delete;

    struct PaneParams
    {
        QRect rect; ///< whole pane, framebuffer pixels
        QPoint contentOrigin; ///< top-left of cell (0, 0)
        bool focused = false;
        bool cursorBlinkOn = true;
        int cursorBlinkOverride = -1; ///< -1 follows terminal mode, 0 steady, 1 blinking
        /// The visible phase of blinking text (SGR 5).
        bool textBlinkOn = true;
        /// Where the cursor is drawn, in pixels from cell (0, 0), while it
        /// glides between cells. Unset draws it at its cell.
        std::optional<QPointF> cursorPosition;
        /// Content shift upwards in pixels, for scrolling by part of a row.
        float scrollShift = 0;
        /// Opacity of the default background (window translucency).
        std::uint8_t backgroundAlpha = 255;
        /// Disabled when text cannot be composited directly onto an opaque background.
        bool allowSubpixel = true;
        /// A link under the pointer, underlined: cells of this snapshot link
        /// index, or a detected URL as a run of cells on one row.
        std::uint32_t hoveredLink = 0;
        int hoverRow = -1;
        int hoverColStart = 0;
        int hoverColEnd = -1; ///< inclusive
        /// Script signals for shaders (uSignal); unset when @c at is empty.
        struct Signal
        {
            float value = 0;
            std::optional<std::chrono::steady_clock::time_point> at;
        };
        std::array<Signal, 8> signals{};
        ShaderParameterMap shaderParameters;
        /// Highlighted in the pane overview.
        bool highlighted = false;
        /// How far a new split has opened (1 when open), and the edge it
        /// opened from: 0 none, 1 left, 2 top, 3 right, 4 bottom.
        float openProgress = 1.0f;
        int openEdge = 0;
        /// Where its shell runs: 0 here, 1 remote, 2 elevated, 3 a container;
        /// where it ran before the last change, so a pack can blend from it;
        /// and how far the last change has run (1 when settled).
        int context = 0;
        int previousContext = 0;
        float contextProgress = 1.0f;
        /// A script's pulse (afterglow.pulse): how far it has run (1 when
        /// done), which effect, and its palette colour.
        float pulseProgress = 1.0f;
        int pulseEffect = 0;
        int pulseColor = 0;
    };

    /// Where each layer's instances start, relative to the pane's first:
    /// backgrounds (the default background first), then text and
    /// decorations, then the cursor (a block cursor and the extra cursors of
    /// its shape group), then from cursorOtherStart the extra cursors of the
    /// other shape group.
    struct Ranges
    {
        std::size_t textStart = 0;
        std::size_t cursorStart = 0;
        std::size_t cursorOtherStart = 0;
        /// A block cursor goes under the text; other shapes go over it.
        bool cursorBelowText = false;
    };

    /// Appends the pane's instances. Atlas pages grow with the visible working
    /// set; allocation failures throw before a partial frame can be submitted.
    /// A glyph the atlas cannot hold is left out.
    void buildPane(const PaneSnapshot& snap, const PaneParams& params, FontSet& fonts, GlyphCache& glyphs,
                   std::vector<Instance>& out, Ranges& ranges);

    void clearShapeCache() { m_shapeCache.clear(); }

private:
    struct ShapedGlyph
    {
        std::uint32_t glyphId;
        std::uint32_t cluster;
        float xOffset; ///< from the start of its cluster
        float yOffset;
        float penX; ///< from the start of the run, offset included
    };
    struct ShapeKey
    {
        int face;
        int slot;
        bool rtl;
        std::uint32_t script;
        std::u32string text;
        bool operator==(const ShapeKey&) const = default;
    };
    struct ShapeKeyHash
    {
        std::size_t operator()(const ShapeKey& k) const noexcept;
    };

    const std::vector<ShapedGlyph>& shape(FontSet& fonts, int face, int slot, const std::u32string& text, bool rtl = false, std::uint32_t script = 0);

    std::uint64_t m_shapeFont = 0;
    std::unordered_map<ShapeKey, std::vector<ShapedGlyph>, ShapeKeyHash> m_shapeCache;
    ShapeKey m_lookup; ///< reused for lookups, so a cache hit copies no text
    std::u32string m_runText;
    std::vector<int> m_runColumns;
    std::vector<int> m_columns;
    std::vector<std::uint32_t> m_clusters;
    hb_buffer_t* m_hbBuffer = nullptr;
};

/// One window's swapchain and pipelines on the shared GpuDevice. Without
/// shaders all panes of the window are drawn in one render pass; with them
/// the grid (or each layer) goes to offscreen textures that PostChain runs
/// its passes over.
class AFTERGLOWRENDER_EXPORT WindowRenderer
{
public:
    struct Pane
    {
        const PaneSnapshot* snapshot = nullptr;
        FrameBuilder::PaneParams params;
    };

    /// @p window may be null for a renderer that only renders to images;
    /// then setDevicePixelRatio() chooses the font scale.
    WindowRenderer(GpuDevice& device, QWindow* window);
    void setDevicePixelRatio(qreal dpr) { m_devicePixelRatio = dpr; }
    ~WindowRenderer();

    WindowRenderer(const WindowRenderer&) = delete;
    WindowRenderer& operator=(const WindowRenderer&) = delete;

    /// Lets the compositor blend the window with what is behind it (the
    /// swapchain carries premultiplied alpha). Recreates the swapchain.
    void setTranslucent(bool translucent);

    /// Creates or resizes the swapchain for the window's current size.
    /// Returns false while the window has no usable surface.
    bool ensureSwapChain();
    void releaseSwapChain();
    QSize pixelSize() const;

    /// Optional diagnostic sampling. GPU time is asynchronous, in seconds;
    /// zero means unavailable. It need not describe the most recent frame.
    void setGpuTimingEnabled(bool enabled) { m_gpuTimingEnabled = enabled; m_completedGpuSeconds = 0; }
    double completedGpuSeconds() const { return m_completedGpuSeconds; }
    std::uint64_t submittedFrames() const { return m_submittedFrames; }


    FontResources& fonts();

    /// Post-processing shaders: over each layer, each pane and the whole window.
    void setShaderChains(ShaderChains chains);
    /// The host keeps scheduling frames while this is true: an always-on
    /// shader animates, the scrollback count eases, the overview or a tab
    /// transition shows, or a pane effect moves.
    bool animating() const;
    /// Another tab's panes, for the overview or the tab a switch leaves.
    struct OverviewTab
    {
        int index = 0;
        std::vector<Pane> panes; ///< laid out in the window, like the current tab's
        /// Solid rectangles drawn over the panes, as render() takes them for
        /// the current tab (split dividers, focus dimming).
        std::vector<std::pair<QRect, std::uint32_t>> overlays;
    };
    /// The pane overview for terminal_panes.glsl, eased by the host. The
    /// overview chain runs while it is open and until it has closed.
    struct Overview
    {
        bool open = false;
        float amount = 0; ///< 0 the flat window, 1 open
        float position = 0; ///< the highlight, as a fractional pane index
        std::optional<std::chrono::steady_clock::time_point> changedAt; ///< opened or closed
        std::optional<std::chrono::steady_clock::time_point> movedAt; ///< the highlight moved
        int currentTab = 0; ///< the index of the tab render() draws
        /// Other tabs, each drawn in turn into the shared tab image and shrunk
        /// into a tile of the atlas; the highlighted pane's tab keeps the full
        /// image.
        std::vector<OverviewTab> otherTabs;
    };
    void setOverview(Overview overview) { m_overview = std::move(overview); }
    /// Open, or still closing.
    bool overviewShowing() const;
    /// A tab switch: the tab being left is drawn at full resolution into
    /// uTabImage while the tab transition chain runs over the new tab.
    struct TabTransition
    {
        bool active = false;
        float progress = 1; ///< 0 the tab being left, 1 the new tab
        float direction = 1; ///< 1 toward a tab on the right, -1 toward one on the left
        std::optional<std::chrono::steady_clock::time_point> startedAt;
        OverviewTab from;
    };
    void setTabTransition(TabTransition transition) { m_transition = std::move(transition); }
    bool transitionShowing() const { return m_transition.active && !overviewShowing(); }
    /// Panes opening and closing: the pane effects chain runs while @c active.
    /// A closed pane's last frame is drawn into uTabImage at its old place.
    struct PaneEffects
    {
        bool active = false;
        bool moving = false; ///< something animates; a static mark needs no new frames
        std::optional<Pane> closing;
        float closingProgress = 1; ///< 0 just closed, 1 gone
    };
    void setPaneEffects(PaneEffects effects) { m_effects = std::move(effects); }
    bool paneEffectsShowing() const { return m_effects.active && !overviewShowing() && !transitionShowing(); }
    /// Synthetic combined rows for developer previews. Unset sums only the rendered panes.
    void setScrollbackLinesOverride(std::optional<std::uint64_t> lines) { m_scrollbackLinesOverride = lines; }
    /// The pointer, in framebuffer pixels, for shaders' iMouse.
    void setMousePosition(const QPointF& px) { m_mouse = px; }

    /// Renders one frame. @p overlays are solid rectangles drawn last
    /// (split dividers, focus dimming).
    /// @p overlayTexture, if given, is a premultiplied full-window texture
    /// (the chrome) drawn over everything; @p overlaySerial changes whenever
    /// that texture object is replaced.
    /// Returns false when nothing was presented (no surface, a swapchain out
    /// of date, a lost device): the host must schedule another frame.
    bool render(const std::vector<Pane>& panes, const std::vector<std::pair<QRect, std::uint32_t>>& overlays,
                std::uint32_t clearColor, QRhiTexture* overlayTexture = nullptr, std::uint64_t overlaySerial = 0);

    /// Shader time (iTime) for the following frames instead of the wall
    /// clock, for reproducible renders; unset returns to the clock. Event
    /// ages (signals, the overview, tab switches) still come from their
    /// steady_clock stamps.
    void setClockOverride(std::optional<double> seconds) { m_clockOverride = seconds; }
    /// Renders the panes into an image instead of the window, through the
    /// same passes and post-processing. For tests and visual checks.
    QImage renderToImage(const QSize& size, const std::vector<Pane>& panes,
                         const std::vector<std::pair<QRect, std::uint32_t>>& overlays, std::uint32_t clearColor);

private:
    bool m_gpuTimingEnabled = false;
    double m_completedGpuSeconds = 0;
    std::uint64_t m_submittedFrames = 0;

    struct PipelineSet
    {
        std::unique_ptr<QRhiGraphicsPipeline> grid;
        std::unique_ptr<QRhiGraphicsPipeline> fill; ///< grid without blending, for pane backgrounds
        std::unique_ptr<QRhiGraphicsPipeline> image;
        std::unique_ptr<QRhiGraphicsPipeline> subpixel;
    };
    /// Where a frame goes: the swapchain or an offscreen texture.
    struct Target
    {
        QSize size;
        std::function<QRhiCommandBuffer*()> begin;
        std::function<QRhiRenderTarget*()> renderTarget; ///< valid after begin
        QRhiRenderPassDescriptor* renderPass = nullptr;
        PipelineSet* pipelines = nullptr;
        /// Recorded at the end of the last pass (an image readback).
        std::function<QRhiResourceUpdateBatch*()> finalUpdates;
        std::function<void()> end;
    };
    void renderTo(const Target& target, const std::vector<Pane>& panes,
                  const std::vector<std::pair<QRect, std::uint32_t>>& overlays, std::uint32_t clearColor,
                  QRhiTexture* overlayTexture, std::uint64_t overlaySerial);
    void initResources(FontResources& res);
    void ensureBindings(FontResources& res);
    enum class GridBlend { None, Premultiplied, Subpixel };
    std::unique_ptr<QRhiGraphicsPipeline> makeGridPipeline(QRhiRenderPassDescriptor* pass, GridBlend blend);
    std::unique_ptr<QRhiGraphicsPipeline> makeImagePipeline(QRhiRenderPassDescriptor* pass,
                                                            QRhiShaderResourceBindings* layout);

    struct ImageDraw
    {
        /// Kitty z: below INT32_MIN/2 under non-default cell backgrounds,
        /// below 0 under the text, else over it.
        enum class Layer { UnderCells, BelowText, AboveText };
        std::uint32_t instance;
        AfterglowVt::FramePtr frame;
        Layer layer;
    };
    struct DrawRange
    {
        std::uint32_t first;
        std::uint32_t textStart;
        std::uint32_t cursorStart;
        std::uint32_t cursorOtherStart;
        std::uint32_t end;
        bool cursorBelowText;
        QRect scissor;
        std::vector<ImageDraw> images;
        std::uint32_t overlayFirst = 0, overlayEnd = 0; ///< another tab's overlays
    };
    QRhiShaderResourceBindings* imageBindings(QRhiTexture* texture, std::uint64_t serial);

    GpuDevice& m_device;
    QWindow* m_window;
    std::unique_ptr<QRhiSwapChain> m_swapChain;
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPass;
    PipelineSet m_swapPipelines;
    PipelineSet m_offscreenPipelines;
    std::unique_ptr<QRhiTexture> m_offscreenTexture;
    std::unique_ptr<QRhiTextureRenderTarget> m_offscreenTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> m_offscreenPass;
    qreal m_devicePixelRatio = 1.0;
    std::optional<double> m_clockOverride;
    std::optional<std::uint64_t> m_scrollbackLinesOverride;
    double m_easedScrollbackLines = 0;
    bool m_scrollbackInitialized = false;
    bool m_scrollbackSettling = false;
    Overview m_overview;
    TabTransition m_transition;
    PaneEffects m_effects;
    PipelineSet m_postPipelines;
    std::optional<std::uint64_t> m_postPipelinePass; ///< PostChain::renderPassGeneration() they were made for
    std::unique_ptr<class PostChain> m_post;
    bool m_postWasActive = false;
    std::uint64_t m_lastPostFrame = 0; ///< m_frameCounter of the last frame with post-processing
    QPointF m_mouse;
    QElapsedTimer m_clock;
    double m_lastFrameTime = 0;
    int m_shaderFrame = 0;
    /// A pane's cursor and focus history for shaders. Cursor rectangles are
    /// in content pixels, before the scroll shift. Times are shader seconds;
    /// -1e6 means never, so a pane that first appears reads no recent change.
    struct CursorTrack
    {
        QRectF cursor;
        QRectF previous;
        QColor color;
        QColor previousColor;
        int style = 0;
        int previousStyle = 0;
        double changeTime = -1e6;
        bool focused = false;
        double focusTime = -1e6;
        std::uint64_t seenFrame = 0;
    };
    std::unordered_map<std::uint64_t, CursorTrack> m_cursorTracks; ///< by PaneSnapshot::identity
    std::vector<std::unique_ptr<QRhiShaderResourceBindings>> m_pageBindings;
    std::unique_ptr<QRhiBuffer> m_uniforms;
    std::unique_ptr<QRhiBuffer> m_instances;
    struct ImageBinding
    {
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        std::uint64_t lastUsed = 0;
    };
    std::unordered_map<std::uint64_t, ImageBinding> m_imageBindings; // by texture serial
    std::uint64_t m_frameCounter = 0;
    /// Textures for this frame's images, by frame.
    std::unordered_map<const AfterglowVt::ImageFrame*, std::pair<QRhiTexture*, std::uint64_t>> m_pendingImages;
    std::uint64_t m_boundTextureGeneration = 0;
    std::uint64_t m_fontEpoch = 0;
    bool m_hasSwapChain = false;
    bool m_translucent = false;

    FrameBuilder m_builder;
    std::vector<Instance> m_frame;
    std::vector<DrawRange> m_ranges;

    /// Each pane's built instances, reused while nothing that shapes them changed.
    struct PaneCache
    {
        std::uint64_t snapshotVersion = 0;
        std::uint64_t fontEpoch = 0;
        std::uint64_t glyphEpoch = 0;
        std::uint64_t fontIdentity = 0; ///< FontSet::identity(): a new set can reuse an address
        QRect rect;
        QPoint origin;
        bool focused = false;
        bool blinkOn = false;
        int cursorBlinkOverride = -1;
        bool textBlinkOn = true;
        std::optional<QPointF> cursorPosition;
        float scrollShift = 0;
        std::uint8_t backgroundAlpha = 0;
        bool allowSubpixel = true;
        std::uint32_t hoveredLink = 0;
        int hoverRow = -1;
        int hoverColStart = 0;
        int hoverColEnd = -1;
        std::vector<Instance> instances;
        FrameBuilder::Ranges ranges;
        std::uint64_t seenFrame = 0;
    };
    std::unordered_map<std::uint64_t, PaneCache> m_paneCache; ///< by PaneSnapshot::identity
    std::vector<Instance> m_scratch;
};

} // namespace AfterglowRender
