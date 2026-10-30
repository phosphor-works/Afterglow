// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/GpuDevice.h>

#include <QLoggingCategory>
#include <QVersionNumber>
#include <QWindow>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

Q_LOGGING_CATEGORY(lcGpu, "afterglow.gpu", QtInfoMsg)

namespace AfterglowRender {

namespace {
GpuDevice* s_instance = nullptr;
}

GpuDevice::GpuDevice(const FontSpec& fontSpec, QObject* parent)
    : QObject(parent)
    , m_fontSpec(fontSpec)
{
    m_vulkan.setApiVersion(QVersionNumber(1, 2));
    if (qEnvironmentVariableIntValue("AFTERGLOW_VULKAN_VALIDATION")) {
        m_vulkan.setLayers({"VK_LAYER_KHRONOS_validation"});
    }
    if (!m_vulkan.create()) {
        throw std::runtime_error("failed to create the Vulkan instance");
    }
    s_instance = this;
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
    s_instance = nullptr;
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

GpuDevice* GpuDevice::instance()
{
    return s_instance;
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
    m_sampler->create();
    m_imageSampler = m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                       QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_imageSampler->create();
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
    QRhiTexture* tex = m_rhi->newTexture(QRhiTexture::RGBA8, size);
    tex->create();
    const QImage img(frame->rgba.data(), size.width(), size.height(), size.width() * 4, QImage::Format_RGBA8888);
    updates->uploadTexture(tex, img);
    ImageTexture entry{frame, tex, ++m_textureSerial};
    serial = entry.serial;
    m_imageTextures.emplace(frame.get(), std::move(entry));
    return tex;
}

void GpuDevice::pruneImageTextures()
{
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
        slot = std::make_unique<FontResources>();
        slot->fonts = std::make_unique<FontSet>(m_fontSpec, devicePixelRatio);
    }
    return *slot;
}

void GpuDevice::prepareAtlases(FontResources& res, QRhiResourceUpdateBatch* updates)
{
    struct PageBinding
    {
        AtlasPage& page;
        QRhiTexture*& texture;
        std::uint64_t& layout;
        QRhiTexture::Format format;
    };
    PageBinding pages[2] = {
        {res.glyphs.grayPage(), res.grayTexture, res.grayLayout, QRhiTexture::R8},
        {res.glyphs.colorPage(), res.colorTexture, res.colorLayout, QRhiTexture::BGRA8},
    };
    for (PageBinding& b : pages) {
        const int size = b.page.size();
        if (!b.texture || b.texture->pixelSize() != QSize(size, size)) {
            delete b.texture;
            b.texture = m_rhi->newTexture(b.format, QSize(size, size));
            b.texture->create();
            res.textureGeneration = ++m_textureSerial;
            b.page.markDirty(QRect(0, 0, size, size));
        }
        b.layout = b.page.layoutVersion();
        const QRect dirty = b.page.takeDirty();
        if (dirty.isEmpty()) {
            continue;
        }
        QRhiTextureSubresourceUploadDescription sub(b.page.image());
        sub.setSourceTopLeft(dirty.topLeft());
        sub.setSourceSize(dirty.size());
        sub.setDestinationTopLeft(dirty.topLeft());
        updates->uploadTexture(b.texture, QRhiTextureUploadDescription({0, 0, sub}));
    }
}

void GpuDevice::releaseFontResources()
{
    for (auto& [key, res] : m_fonts) {
        delete res->grayTexture;
        delete res->colorTexture;
    }
    m_fonts.clear();
}

void GpuDevice::setFontSpec(const FontSpec& spec)
{
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
    spec.sizePoints = std::clamp(spec.sizePoints + deltaPoints, 4.0f, 96.0f);
    if (spec.sizePoints != m_fontSpec.sizePoints) {
        setFontSpec(spec);
    }
}

} // namespace AfterglowRender
