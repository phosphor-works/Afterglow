// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/GpuDevice.h>

#include <QLoggingCategory>
#include <QVersionNumber>
#include <QWindow>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

Q_LOGGING_CATEGORY(lcGpu, "afterglow.gpu", QtInfoMsg)

namespace AfterglowRender {

GpuDevice::GpuDevice(const FontSpec& fontSpec, QObject* parent)
    : QObject(parent)
    , m_fontSpec(fontSpec)
{
    m_vulkan.setApiVersion(QVersionNumber(1, 2));
    // QRhi can select Wayland surface color spaces that require instance
    // extensions. QVulkanInstance filters out extensions the loader lacks.
    m_vulkan.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
    if (qEnvironmentVariableIntValue("AFTERGLOW_VULKAN_VALIDATION")) {
        m_vulkan.setLayers({"VK_LAYER_KHRONOS_validation"});
    }
    if (!m_vulkan.create()) {
        throw std::runtime_error("failed to create the Vulkan instance");
    }
}

GpuDevice::~GpuDevice()
{
    releaseFontResources();
    for (auto& [frame, entry] : m_imageTextures) {
        delete entry.texture;
    }
    m_imageTextures.clear();
    delete m_audioTexture;
    delete m_imageSampler;
    delete m_sampler;
    m_rhi.reset();
}

void GpuDevice::setAudioSpectrum(std::span<const float> spectrum)
{
    std::array<float, AudioBars> next{};
    const int size = spectrum.size() == AudioBars ? AudioBars : 0;
    for (int i = 0; i < size; ++i) {
        next[i] = std::isfinite(spectrum[i]) ? std::clamp(spectrum[i], 0.0f, 1.0f) : 0.0f;
    }
    if (next == m_audioSpectrum && size == m_audioSize) {
        return;
    }
    m_audioSpectrum = next;
    m_audioSize = size;
    // RMS bands preserve narrow transients without a single-bin peak flicker.
    const auto rms = [&next](int begin, int end) {
        float energy = 0;
        for (int i = begin; i < end; ++i) {
            energy += next[i] * next[i];
        }
        return std::sqrt(energy / float(end - begin));
    };
    m_audioLevels = {rms(0, 20), rms(20, 48), rms(48, 64), rms(0, 64)};
    m_audioDirty = true;
    Q_EMIT audioChanged();
}

void GpuDevice::prepareAudio(QRhiResourceUpdateBatch* updates)
{
    if (!m_audioTexture) {
        m_audioTexture = m_rhi->newTexture(QRhiTexture::R32F, QSize(AudioBars, 1));
        if (!m_audioTexture->create()) {
            throw std::runtime_error("failed to create the audio spectrum texture");
        }
        m_audioDirty = true;
    }
    if (m_audioDirty) {
        const QByteArray bytes(reinterpret_cast<const char*>(m_audioSpectrum.data()), sizeof(m_audioSpectrum));
        updates->uploadTexture(m_audioTexture, QRhiTextureUploadDescription({
            QRhiTextureUploadEntry(0, 0, QRhiTextureSubresourceUploadDescription(bytes))}));
        m_audioDirty = false;
    }
}

QRhi* GpuDevice::rhi(QWindow* window)
{
    if (m_rhi) {
        return m_rhi.get();
    }
    QRhiVulkanInitParams params;
    params.inst = &m_vulkan;
    params.window = window;
    m_rhi.reset(QRhi::create(QRhi::Vulkan, &params, QRhi::EnableTimestamps));
    if (!m_rhi) {
        throw std::runtime_error("failed to create the Vulkan device");
    }
    qCInfo(lcGpu) << "Vulkan device:" << m_rhi->driverInfo().deviceName;

    m_sampler = m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                  QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    if (!m_sampler->create()) throw std::runtime_error("failed to create the atlas sampler");
    m_imageSampler = m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                       QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    if (!m_imageSampler->create()) throw std::runtime_error("failed to create the image sampler");
    return m_rhi.get();
}

QRhiTexture* GpuDevice::imageTexture(const AfterglowVt::FramePtr& frame, QRhiResourceUpdateBatch* updates,
                                     std::uint64_t& serial)
{
    auto it = m_imageTextures.find(frame.get());
    // The address alone could belong to a freed frame: check it is the same one.
    if (it != m_imageTextures.end() && it->second.frame.lock() == frame) {
        serial = it->second.serial;
        return it->second.texture;
    }
    if (it != m_imageTextures.end()) {
        delete it->second.texture;
        m_imageTextures.erase(it);
    }
    const QSize size(int(frame->width), int(frame->height));
    const int limit = m_rhi->resourceLimit(QRhi::TextureSizeMax);
    std::unique_ptr<QRhiTexture> created;
    if (!size.isEmpty() && size.width() <= limit && size.height() <= limit) {
        created.reset(m_rhi->newTexture(QRhiTexture::RGBA8, size));
        if (!created->create()) created.reset();
    }
    if (!created) {
        if (!std::exchange(m_imageLimitWarned, true))
            qCWarning(lcGpu) << "An image of" << size << "does not fit on the GPU (limit" << limit << "pixels); it is not drawn";
        return nullptr;
    }
    QRhiTexture* tex = created.release();
    const QImage img(frame->rgba.data(), size.width(), size.height(), size.width() * 4, QImage::Format_RGBA8888);
    updates->uploadTexture(tex, img);
    ImageTexture entry{frame, tex, ++m_textureSerial};
    serial = entry.serial;
    m_imageTextures.emplace(frame.get(), std::move(entry));
    return tex;
}

void GpuDevice::pruneUnused()
{
    // Font sets for a pixel ratio no window uses any more (a window moved to
    // another screen, a scale that changed).
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_fonts.begin(); it != m_fonts.end();) {
        if (now - it->second->lastUsed > FontResourcesUnusedFor) {
            for (auto* texture : it->second->grayTextures) delete texture;
            for (auto* texture : it->second->colorTextures) delete texture;
            it = m_fonts.erase(it);
        } else {
            ++it;
        }
    }
    // QRhi defers the native release until in-flight frames are done with it.
    for (auto it = m_imageTextures.begin(); it != m_imageTextures.end();) {
        if (it->second.frame.expired()) {
            delete it->second.texture;
            it = m_imageTextures.erase(it);
        } else {
            ++it;
        }
    }
}

FontResources& GpuDevice::fontResources(double devicePixelRatio)
{
    const int key = int(std::lround(devicePixelRatio * 100.0));
    auto& slot = m_fonts[key];
    if (!slot) {
        // Stored only once the fonts loaded: a throw leaves no half-made entry.
        auto made = std::make_unique<FontResources>();
        made->fonts = std::make_unique<FontSet>(m_fontSpec, devicePixelRatio);
        slot = std::move(made);
    }
    slot->lastUsed = std::chrono::steady_clock::now();
    return *slot;
}

void GpuDevice::prepareAtlases(FontResources& res, QRhiResourceUpdateBatch* updates)
{
    auto upload = [&](bool color) {
        auto& textures = color ? res.colorTextures : res.grayTextures;
        const auto count = color ? res.glyphs.colorPageCount() : res.glyphs.grayPageCount();
        while (textures.size() > count) { delete textures.back(); textures.pop_back(); res.textureGeneration = ++m_textureSerial; }
        textures.resize(count, nullptr);
        for (std::size_t i = 0; i < count; ++i) {
            auto& page = color ? res.glyphs.colorPage(i) : res.glyphs.grayPage(i);
            auto*& texture = textures[i];
            const QSize size = page.image().size();
            if (!texture || texture->pixelSize() != size) {
                delete texture;
                texture = m_rhi->newTexture(color ? QRhiTexture::BGRA8 : QRhiTexture::R8, size);
                if (!texture->create()) throw std::runtime_error("glyph atlas GPU allocation failed");
                res.textureGeneration = ++m_textureSerial;
                page.markDirty(page.image().rect());
            }
            const std::vector<QRect> dirty = page.takeDirty();
            if (dirty.empty()) continue;
            // One entry per new region, not their bounding box across shelves.
            // Each entry carries an image of its region only: QRhi reserves
            // staging memory for an entry's whole source image, so 32 entries
            // over a 4096 px colour page would ask for 2 GiB.
            std::vector<QRhiTextureUploadEntry> entries;
            entries.reserve(dirty.size());
            for (const QRect& rect : dirty) {
                QRhiTextureSubresourceUploadDescription sub(
                    rect == page.image().rect() ? page.image() : page.image().copy(rect));
                sub.setDestinationTopLeft(rect.topLeft());
                entries.emplace_back(0, 0, sub);
            }
            QRhiTextureUploadDescription description;
            description.setEntries(entries.begin(), entries.end());
            updates->uploadTexture(texture, description);
        }
    };
    upload(false); upload(true);
}

void GpuDevice::releaseFontResources()
{
    for (auto& [key, res] : m_fonts) {
        for (auto* texture : res->grayTextures) delete texture;
        for (auto* texture : res->colorTextures) delete texture;
    }
    m_fonts.clear();
}

void GpuDevice::setFontSpec(const FontSpec& spec)
{
    FontSpec sameGamma = spec;
    sameGamma.gamma = m_fontSpec.gamma;
    const bool gammaSame = spec.gamma == m_fontSpec.gamma || (std::isnan(spec.gamma) && std::isnan(m_fontSpec.gamma));
    if (sameGamma == m_fontSpec) {
        if (gammaSame) return;
        // Gamma is read per frame (the grid's uniforms): nothing to rebuild.
        m_fontSpec = spec;
        Q_EMIT fontsChanged();
        return;
    }
    m_fontSpec = spec;
    // Textures may still be referenced by in-flight frames; QRhi defers the
    // actual release until the GPU is done with them.
    releaseFontResources();
    ++m_fontEpoch;
    Q_EMIT fontsChanged();
}

void GpuDevice::adjustFontSize(float deltaPoints)
{
    FontSpec spec = m_fontSpec;
    spec.sizePoints = std::clamp(spec.sizePoints + deltaPoints, FontSpec::MinPoints, FontSpec::MaxPoints);
    if (spec.sizePoints != m_fontSpec.sizePoints) {
        setFontSpec(spec);
    }
}

} // namespace AfterglowRender
