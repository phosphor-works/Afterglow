// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/AudioController.h"

#include <QCoreApplication>
#include <QEventLoop>

#include <gtest/gtest.h>

using namespace Afterglow;
using namespace PhosphorAudio;

namespace {
class Provider : public IAudioSpectrumProvider
{
public:
    bool isAvailable() const override { return available; }
    void start() override
    {
        ++starts;
        if (!available) {
            Q_EMIT errorOccurred(QStringLiteral("test backend unavailable"));
            return;
        }
        running = true;
        Q_EMIT runningChanged(true);
    }
    void stop() override
    {
        if (running) ++stops;
        running = false;
        Q_EMIT runningChanged(false);
    }
    bool isRunning() const override { return running; }
    SpectrumOptions options() const override { return opts; }
    void setOptions(const SpectrumOptions& options) override { opts = options; }
    QVector<float> spectrum() const override { return {}; }
    void frame(float value) { Q_EMIT spectrumUpdated(QVector<float>(64, value)); }

    bool available = true;
    bool running = false;
    int starts = 0;
    int stops = 0;
    SpectrumOptions opts;
};

class Audio : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto provider = std::make_unique<Provider>();
        source = provider.get();
        controller = std::make_unique<AudioController>(std::move(provider));
        QObject::connect(controller.get(), &AudioController::spectrumChanged, controller.get(),
                         [this](const QVector<float>& frame) { spectrum = frame; });
    }
    static void flush() { QCoreApplication::processEvents(); }
    Provider* source = nullptr;
    QVector<float> spectrum;
    std::unique_ptr<AudioController> controller;
};
} // namespace

TEST_F(Audio, CapturesOnlyWhileAtLeastOneConsumerNeedsIt)
{
    flush();
    EXPECT_EQ(source->starts, 0);
    EXPECT_EQ(source->opts.channelMode, ChannelMode::MonoAverage);
    EXPECT_EQ(source->opts.barCount, 64);
    QObject a;
    auto b = std::make_unique<QObject>();
    controller->setActive(&a, true);
    controller->setActive(b.get(), true);
    flush();
    EXPECT_EQ(source->starts, 1);
    source->frame(0.75f);
    EXPECT_EQ(spectrum, QVector<float>(64, 0.75f));
    controller->setActive(&a, false);
    flush();
    EXPECT_TRUE(source->isRunning());
    b.reset();
    flush();
    EXPECT_FALSE(source->isRunning());
    EXPECT_TRUE(spectrum.isEmpty());
    source->frame(1.0f); // A queued frame after stopping must be ignored.
    EXPECT_TRUE(spectrum.isEmpty());
}

TEST_F(Audio, CoalescesVisibilityChangesAndLiveReloadsSettings)
{
    QObject window;
    controller->setActive(&window, true);
    controller->setActive(&window, false);
    flush();
    EXPECT_EQ(source->starts, 0);
    controller->setActive(&window, true);
    flush();
    source->frame(0.5f);
    AudioSettings settings;
    settings.enabled = false;
    settings.inputMethod = QStringLiteral("pulse");
    settings.inputSource = QStringLiteral("speakers.monitor");
    controller->configure(settings);
    flush();
    EXPECT_FALSE(source->isRunning());
    EXPECT_TRUE(spectrum.isEmpty());
    EXPECT_EQ(source->starts, 1);
    source->frame(1.0f);
    EXPECT_TRUE(spectrum.isEmpty());
    settings.enabled = true;
    controller->configure(settings);
    flush();
    EXPECT_EQ(source->starts, 2);
    EXPECT_EQ(source->opts.inputMethod, QStringLiteral("pulse"));
    EXPECT_EQ(source->opts.inputSource, QStringLiteral("speakers.monitor"));
    controller->configure(settings);
    flush();
    EXPECT_EQ(source->starts, 2);
}

TEST_F(Audio, UnavailableBackendDoesNotRespawnUntilDemandChanges)
{
    QObject window;
    source->available = false;
    controller->setActive(&window, true);
    flush();
    controller->setActive(&window, true);
    flush();
    EXPECT_EQ(source->starts, 1);
    EXPECT_TRUE(spectrum.isEmpty());
    controller->setActive(&window, false);
    flush();
    source->available = true;
    controller->setActive(&window, true);
    flush();
    EXPECT_EQ(source->starts, 2);
    EXPECT_TRUE(source->isRunning());
}

TEST_F(Audio, ClearsStalledAndFailedCapture)
{
    QObject window;
    controller->setActive(&window, true);
    flush();
    source->frame(0.8f);
    ASSERT_FALSE(spectrum.isEmpty());
    QEventLoop loop;
    QTimer::singleShot(1150, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT_TRUE(spectrum.isEmpty());
    source->frame(0.4f);
    EXPECT_FALSE(spectrum.isEmpty());
    Q_EMIT source->errorOccurred(QStringLiteral("test capture failure"));
    EXPECT_TRUE(spectrum.isEmpty());
    EXPECT_EQ(source->starts, 1);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
