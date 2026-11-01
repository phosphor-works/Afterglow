// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AudioController.h"

#include <PhosphorAudio/CavaSpectrumProvider.h>

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcAudio, "afterglow.audio", QtInfoMsg)

namespace Afterglow {

AudioController::AudioController(QObject* parent)
    : AudioController(std::make_unique<PhosphorAudio::CavaSpectrumProvider>(), parent)
{
}

AudioController::AudioController(std::unique_ptr<PhosphorAudio::IAudioSpectrumProvider> provider, QObject* parent)
    : QObject(parent)
    , m_provider(std::move(provider))
{
    m_reconcile.setSingleShot(true);
    m_reconcile.setInterval(0);
    connect(&m_reconcile, &QTimer::timeout, this, &AudioController::reconcile);
    m_stale.setSingleShot(true);
    m_stale.setInterval(1000);
    connect(&m_stale, &QTimer::timeout, this, &AudioController::clear);
    connect(m_provider.get(), &PhosphorAudio::IAudioSpectrumProvider::spectrumUpdated, this,
            [this](const QVector<float>& spectrum) {
        if (m_requested && m_settings.enabled && !m_consumers.isEmpty()) {
            m_stale.start();
            m_cleared = spectrum.isEmpty();
            Q_EMIT spectrumChanged(spectrum);
        }
    });
    // The analyzer stopped by itself (CAVA crashed or failed): the next
    // visibility or configuration change starts it again, never a loop here.
    connect(m_provider.get(), &PhosphorAudio::IAudioSpectrumProvider::runningChanged, this, [this](bool running) {
        if (running) return;
        m_requested = false;
        clear();
    });
    connect(m_provider.get(), &PhosphorAudio::IAudioSpectrumProvider::errorOccurred, this, [this](const QString& message) {
        m_requested = false;
        clear();
        qCWarning(lcAudio).noquote() << message;
    });
    configure(m_settings);
}

AudioController::~AudioController()
{
    m_requested = false;
    // No callbacks into this controller during member destruction.
    disconnect(m_provider.get(), nullptr, this, nullptr);
    m_provider->stop();
}

void AudioController::configure(const AudioSettings& settings)
{
    const bool changed = settings != m_settings;
    m_settings = settings;
    // Stop before changing options so disabling cannot restart the analyzer.
    if (changed) {
        m_requested = false;
        m_provider->stop();
        clear();
    }
    PhosphorAudio::SpectrumOptions options;
    options.barCount = 64; // the bins of the shader contract's audio uniforms
    options.framerate = settings.framerate;
    options.channelMode = PhosphorAudio::ChannelMode::MonoAverage;
    options.noiseReduction = settings.smoothing;
    options.extraSmoothing = settings.extraSmoothing;
    options.lowerCutoffHz = settings.lowerCutoff;
    options.higherCutoffHz = settings.higherCutoff;
    options.inputMethod = PhosphorAudio::inputMethodFromSetting(settings.inputMethod);
    options.inputSource = settings.inputSource;
    m_provider->setOptions(options);
    m_reconcile.start();
}

void AudioController::setActive(QObject* consumer, bool active)
{
    if (active && !m_consumers.contains(consumer)) {
        m_consumers.insert(consumer, connect(consumer, &QObject::destroyed, this, [this, consumer] {
            m_consumers.remove(consumer);
            m_reconcile.start();
        }));
    } else if (!active && m_consumers.contains(consumer)) {
        disconnect(m_consumers.take(consumer));
    } else {
        return;
    }
    m_reconcile.start();
}

void AudioController::reconcile()
{
    const bool wanted = m_settings.enabled && !m_consumers.isEmpty();
    if (wanted == m_requested) return;
    m_requested = wanted;
    if (wanted) {
        m_provider->start();
    } else {
        m_provider->stop();
        clear();
    }
}

void AudioController::clear()
{
    m_stale.stop();
    if (m_cleared) return;
    m_cleared = true;
    Q_EMIT spectrumChanged({});
}

} // namespace Afterglow
