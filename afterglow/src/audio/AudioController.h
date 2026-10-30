// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "AudioSettings.h"

#include <PhosphorAudio/IAudioSpectrumProvider.h>

#include <QHash>
#include <QTimer>

#include <memory>

namespace Afterglow {

/// One system-playback analyzer shared by all visible audio-reactive windows.
class AudioController : public QObject
{
    Q_OBJECT

public:
    explicit AudioController(QObject* parent = nullptr);
    explicit AudioController(std::unique_ptr<PhosphorAudio::IAudioSpectrumProvider> provider,
                             QObject* parent = nullptr);
    ~AudioController() override;

    void configure(const AudioSettings& settings);
    void setActive(QObject* consumer, bool active);

Q_SIGNALS:
    /// Empty means unavailable, disabled, stopped or stale; never retain old audio.
    void spectrumChanged(const QVector<float>& spectrum);

private:
    void reconcile();
    void clear();

    std::unique_ptr<PhosphorAudio::IAudioSpectrumProvider> m_provider;
    AudioSettings m_settings;
    QHash<QObject*, QMetaObject::Connection> m_consumers;
    QTimer m_reconcile;
    QTimer m_stale;
    bool m_requested = false;
};

} // namespace Afterglow
