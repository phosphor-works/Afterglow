// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/FontSet.h>
#include <AfterglowRender/GlyphCache.h>
#include <AfterglowRender/afterglowrender_export.h>

#include <AfterglowVt/Graphics.h>

#include <QObject>
#include <QVulkanInstance>

#include <array>
#include <chrono>
#include <span>
#include <map>
#include <memory>
#include <unordered_map>

class QRhi;
class QRhiResourceUpdateBatch;
class QRhiTexture;
class QRhiSampler;
class QWindow;

namespace AfterglowRender {

/// Fonts, glyph cache and atlas textures for one device pixel ratio.
struct FontResources
{
    std::unique_ptr<FontSet> fonts;
    GlyphCache glyphs;
    std::vector<QRhiTexture*> grayTextures, colorTextures;
    /// Changes whenever a texture object is replaced; bindings must be rebuilt.
    /// Unique across all FontResources, so a reused address never aliases.
    std::uint64_t textureGeneration = 0;
    /// When a window last asked for these resources.
    std::chrono::steady_clock::time_point lastUsed;
};

/// The single GPU context of the application: one Vulkan instance, one QRhi
/// device, and the font and glyph resources every window and pane shares.
/// Construct after QGuiApplication, destroy after every window.
class AFTERGLOWRENDER_EXPORT GpuDevice : public QObject
{
    Q_OBJECT

public:
    explicit GpuDevice(const FontSpec& fontSpec, QObject* parent = nullptr);
    ~GpuDevice() override;

    QVulkanInstance* vulkanInstance() noexcept { return &m_vulkan; }
    /// The shared device, created on first use. @p window picks a queue that
    /// can present to it.
    QRhi* rhi(QWindow* window);
    QRhi* rhi() const noexcept { return m_rhi.get(); }
    QRhiSampler* atlasSampler() const noexcept { return m_sampler; }

    /// The resources for @p devicePixelRatio, made on first use. A set no
    /// window has asked for in FontResourcesUnusedFor is freed by
    /// pruneUnused(); a window compares FontSet::identity(), not the
    /// address, to see that it changed.
    FontResources& fontResources(double devicePixelRatio);
    static constexpr std::chrono::seconds FontResourcesUnusedFor{30};
    /// Creates or grows the atlas textures and records pending glyph uploads.
    void prepareAtlases(FontResources& res, QRhiResourceUpdateBatch* updates);

    /// Texture for an image frame, uploaded on first use and shared by every
    /// window. @p serial receives an id that changes if the texture is replaced.
    /// Null when the frame is larger than the GPU allows or the texture cannot
    /// be made: the image is not drawn.
    QRhiTexture* imageTexture(const AfterglowVt::FramePtr& frame, QRhiResourceUpdateBatch* updates,
                              std::uint64_t& serial);
    QRhiSampler* imageSampler() const noexcept { return m_imageSampler; }
    /// Releases textures of frames nobody references any more, and font
    /// resources no window uses. Call once per frame.
    void pruneUnused();

    const FontSpec& fontSpec() const noexcept { return m_fontSpec; }
    /// Bumped on every font change; face indices from before are invalid.
    std::uint64_t fontEpoch() const noexcept { return m_fontEpoch; }
    /// Rebuilds the fonts when @p spec differs in more than the gamma, which is
    /// only a shader input: then windows just render again. Emits
    /// fontsChanged() only for a change.
    void setFontSpec(const FontSpec& spec);
    void adjustFontSize(float deltaPoints);

    /// Mono, low-to-high logarithmic spectrum (50 Hz–10 kHz), 64 bars.
    static constexpr int AudioBars = 64;
    void setAudioSpectrum(std::span<const float> spectrum);
    void prepareAudio(QRhiResourceUpdateBatch* updates);
    QRhiTexture* audioTexture() const noexcept { return m_audioTexture; }
    const std::array<float, 4>& audioLevels() const noexcept { return m_audioLevels; }
    int audioSpectrumSize() const noexcept { return m_audioSize; }

Q_SIGNALS:
    void audioChanged();
    void fontsChanged();

private:
    void releaseFontResources();

    std::array<float, AudioBars> m_audioSpectrum{};
    std::array<float, 4> m_audioLevels{};
    int m_audioSize = 0;
    bool m_audioDirty = true;
    QRhiTexture* m_audioTexture = nullptr;
    FontSpec m_fontSpec;
    QVulkanInstance m_vulkan;
    std::unique_ptr<QRhi> m_rhi;
    QRhiSampler* m_sampler = nullptr;
    QRhiSampler* m_imageSampler = nullptr;

    struct ImageTexture
    {
        std::weak_ptr<const AfterglowVt::ImageFrame> frame;
        QRhiTexture* texture = nullptr;
        std::uint64_t serial = 0;
    };
    std::unordered_map<const AfterglowVt::ImageFrame*, ImageTexture> m_imageTextures;
    bool m_imageLimitWarned = false;
    std::map<int, std::unique_ptr<FontResources>> m_fonts; // keyed by dpr * 100
    std::uint64_t m_textureSerial = 0;
    std::uint64_t m_fontEpoch = 1;
};

} // namespace AfterglowRender
