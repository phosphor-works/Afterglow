// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/ShaderProgram.h>
#include <AfterglowRender/afterglowrender_export.h>

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QColor>
#include <QSize>

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
class QColor;
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
};

enum class InstanceKind : std::uint32_t {
    Solid = 0,
    GrayGlyph = 1,
    ColorGlyph = 2,
    Undercurl = 3,
    Dotted = 4,
    Dashed = 5,
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
        /// Where the cursor is drawn, in pixels from cell (0, 0), while it
        /// glides between cells. Unset draws it at its cell.
        std::optional<QPointF> cursorPosition;
        /// Content shift upwards in pixels, for scrolling by part of a row.
        float scrollShift = 0;
        /// Opacity of the default background (window translucency).
        std::uint8_t backgroundAlpha = 255;
        /// A link under the pointer, underlined: cells of this snapshot link
        /// index, or a detected URL as a run of cells on one row.
        std::uint32_t hoveredLink = 0;
        int hoverRow = -1;
        int hoverColStart = 0;
        int hoverColEnd = -1; ///< inclusive
    };

    /// Appends the pane's instances. Returns false if the glyph atlas
    /// overflowed; the caller resets the glyph cache and rebuilds the frame.
    /// @p textStart receives the index where the text layer begins (after the
    /// backgrounds and a block cursor), so images can be drawn in between.
    bool buildPane(const PaneSnapshot& snap, const PaneParams& params, FontSet& fonts, GlyphCache& glyphs,
                   std::vector<Instance>& out, std::size_t& textStart);

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
        std::u32string text;
        bool operator==(const ShapeKey&) const = default;
    };
    struct ShapeKeyHash
    {
        std::size_t operator()(const ShapeKey& k) const noexcept;
    };

    const std::vector<ShapedGlyph>& shape(FontSet& fonts, int face, int slot, const std::u32string& text);

    std::unordered_map<ShapeKey, std::vector<ShapedGlyph>, ShapeKeyHash> m_shapeCache;
    std::u32string m_runText;
    std::vector<int> m_runColumns;
    hb_buffer_t* m_hbBuffer = nullptr;
};

/// One window's swapchain and pipeline on the shared GpuDevice. All panes of
/// the window are drawn in a single render pass.
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

    FontResources& fonts();

    /// Post-processing shaders run over each pane and over the whole window.
    void setShaderChains(std::vector<ShaderProgramPtr> pane, std::vector<ShaderProgramPtr> window);
    /// A shader animates over time: the host keeps scheduling frames.
    bool animating() const;
    /// The pointer, in framebuffer pixels, for shaders' iMouse.
    void setMousePosition(const QPointF& px) { m_mouse = px; }

    /// Renders one frame. @p overlays are solid rectangles drawn last
    /// (split dividers, focus dimming).
    /// @p overlayTexture, if given, is a premultiplied full-window texture
    /// (the chrome) drawn over everything; @p overlaySerial changes whenever
    /// that texture object is replaced.
    void render(const std::vector<Pane>& panes, const std::vector<std::pair<QRect, std::uint32_t>>& overlays,
                std::uint32_t clearColor, QRhiTexture* overlayTexture = nullptr, std::uint64_t overlaySerial = 0);

    /// Renders the panes into an image instead of the window, through the
    /// same passes and post-processing. For tests and visual checks.
    /// Shader time for the following frames instead of the wall clock, for
    /// reproducible renders; unset returns to the clock.
    void setClockOverride(std::optional<float> seconds) { m_clockOverride = seconds; }
    QImage renderToImage(const QSize& size, const std::vector<Pane>& panes,
                         const std::vector<std::pair<QRect, std::uint32_t>>& overlays, std::uint32_t clearColor);

private:
    struct PipelineSet
    {
        std::unique_ptr<QRhiGraphicsPipeline> grid;
        std::unique_ptr<QRhiGraphicsPipeline> fill; ///< grid without blending, for pane backgrounds
        std::unique_ptr<QRhiGraphicsPipeline> image;
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
    std::unique_ptr<QRhiGraphicsPipeline> makeGridPipeline(QRhiRenderPassDescriptor* pass, bool blend);
    std::unique_ptr<QRhiGraphicsPipeline> makeImagePipeline(QRhiRenderPassDescriptor* pass,
                                                            QRhiShaderResourceBindings* layout);

    struct ImageDraw
    {
        std::uint32_t instance;
        AfterglowVt::FramePtr frame;
        bool belowText;
    };
    struct DrawRange
    {
        std::uint32_t first;
        std::uint32_t textStart;
        std::uint32_t end;
        QRect scissor;
        std::vector<ImageDraw> images;
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
    std::optional<float> m_clockOverride;
    PipelineSet m_postPipelines;
    QRhiRenderPassDescriptor* m_postPipelinePass = nullptr;
    std::unique_ptr<class PostChain> m_post;
    QPointF m_mouse;
    QElapsedTimer m_clock;
    float m_lastFrameTime = 0;
    int m_shaderFrame = 0;
    struct CursorTrack
    {
        QRectF cursor;
        QRectF previous;
        QColor color;
        QColor previousColor;
        int style = 0;
        int previousStyle = 0;
        float changeTime = 0;
        bool focused = false;
        float focusTime = 0;
        std::uint64_t seenFrame = 0;
    };
    std::unordered_map<const PaneSnapshot*, CursorTrack> m_cursorTracks;
    std::unique_ptr<QRhiShaderResourceBindings> m_bindings;
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
        const FontSet* fonts = nullptr;
        QRect rect;
        QPoint origin;
        bool focused = false;
        bool blinkOn = false;
        std::optional<QPointF> cursorPosition;
        float scrollShift = 0;
        std::uint8_t backgroundAlpha = 0;
        std::uint32_t hoveredLink = 0;
        int hoverRow = -1;
        int hoverColStart = 0;
        int hoverColEnd = -1;
        std::vector<Instance> instances;
        std::size_t textStart = 0;
        std::uint64_t seenFrame = 0;
    };
    std::unordered_map<const PaneSnapshot*, PaneCache> m_paneCache;
    std::vector<Instance> m_scratch;
};

} // namespace AfterglowRender
