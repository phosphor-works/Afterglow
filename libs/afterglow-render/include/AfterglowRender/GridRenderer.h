// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/afterglowrender_export.h>

#include <QRect>
#include <QSize>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiShaderResourceBindings;
class QRhiTexture;
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

    WindowRenderer(GpuDevice& device, QWindow* window);
    ~WindowRenderer();

    WindowRenderer(const WindowRenderer&) = delete;
    WindowRenderer& operator=(const WindowRenderer&) = delete;

    /// Creates or resizes the swapchain for the window's current size.
    /// Returns false while the window has no usable surface.
    bool ensureSwapChain();
    void releaseSwapChain();
    QSize pixelSize() const;

    FontResources& fonts();

    /// Renders one frame. @p overlays are solid rectangles drawn last
    /// (split dividers, focus dimming).
    void render(const std::vector<Pane>& panes, const std::vector<std::pair<QRect, std::uint32_t>>& overlays,
                std::uint32_t clearColor);

private:
    void createPipeline(FontResources& res);
    void ensureBindings(FontResources& res);

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
    void createImagePipeline(QRhiShaderResourceBindings* layout);
    QRhiShaderResourceBindings* imageBindings(QRhiTexture* texture, std::uint64_t serial);

    GpuDevice& m_device;
    QWindow* m_window;
    std::unique_ptr<QRhiSwapChain> m_swapChain;
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPass;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
    std::unique_ptr<QRhiShaderResourceBindings> m_bindings;
    std::unique_ptr<QRhiBuffer> m_uniforms;
    std::unique_ptr<QRhiBuffer> m_instances;
    std::unique_ptr<QRhiGraphicsPipeline> m_imagePipeline;
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

    FrameBuilder m_builder;
    std::vector<Instance> m_frame;
    std::vector<DrawRange> m_ranges;
};

} // namespace AfterglowRender
