// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaders/ShaderControls.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <gtest/gtest.h>
#include <limits>
using namespace Afterglow;
using namespace AfterglowRender;

class ShaderControlsTest : public ::testing::Test
{
protected:
    ShaderLibrary library;
    ShaderControls controls{library};
    ShaderChains chains;
    void SetUp() override
    {
        ShaderLibrary::Refs refs;
        refs.background = {QStringLiteral("bloom"), QStringLiteral("bloom")};
        refs.window = {QStringLiteral("bloom")};
        chains = library.build(refs, {{QStringLiteral("bloom"), QVariantMap{{QStringLiteral("strength"), 0.4}}}});
        ASSERT_EQ(chains.background.size(), 2);
        ASSERT_EQ(chains.window.size(), 1);
        controls.rebuild(chains);
    }
    QString id(int index = 0) { return controls.describe(7).at(index).toMap().value(QStringLiteral("instance")).toString(); }
    double strength(quint32 pane = 7, int index = 0) {
        return controls.describe(pane).at(index).toMap().value(QStringLiteral("values")).toMap().value(QStringLiteral("strength")).toDouble();
    }
    QString update(QString owner, QList<quint32> panes, QVariantMap values, int index = 0, QString scope = QStringLiteral("pane")) {
        return controls.update(owner, panes, id(index), scope, values);
    }
};

TEST_F(ShaderControlsTest, InstancesAreDistinctAndUpdatesNeverRecompile)
{
    EXPECT_NE(id(), id(1));
    const auto* program = chains.background[0].get();
    const auto shader = program->shader();
    ASSERT_TRUE(update("one", {7, 8}, {{"strength", 1.2}}, 0, "tab").isEmpty());
    EXPECT_NEAR(strength(), 1.2, 0.0001);
    EXPECT_NEAR(strength(8), 1.2, 0.0001);
    EXPECT_NEAR(strength(9), 0.4, 0.0001);
    EXPECT_NEAR(strength(7, 1), 0.4, 0.0001);
    EXPECT_EQ(chains.background[0].get(), program);
    EXPECT_EQ(chains.background[0]->shader(), shader);
}
TEST_F(ShaderControlsTest, RejectsInvalidBatchWithoutPartialChanges)
{
    for (const QVariant& value : {QVariant(-1.), QVariant(3.), QVariant("1.2"), QVariant(true),
         QVariant(std::numeric_limits<double>::infinity()), QVariant(std::numeric_limits<double>::quiet_NaN())}) {
        EXPECT_FALSE(update("one", {7, 8}, {{"radius", 12.}, {"strength", value}}, 0, "tab").isEmpty());
        EXPECT_NEAR(strength(), 0.4, 0.0001);
        EXPECT_TRUE(controls.values(8).isEmpty());
    }
    EXPECT_FALSE(update("one", {7}, {{"unknown", 1.}}).isEmpty());
    EXPECT_FALSE(update("one", {7}, {{"strength", 1.}}, 0, "bogus").isEmpty());
    EXPECT_FALSE(update("one", {}, {{"strength", 1.}}).isEmpty());
}
TEST_F(ShaderControlsTest, OwnerPrecedenceResetAndCleanup)
{
    ASSERT_TRUE(update("one", {7}, {{"strength", 0.7}, {"radius", 10.}}).isEmpty());
    ASSERT_TRUE(update("two", {7}, {{"strength", 1.4}}).isEmpty());
    EXPECT_NEAR(strength(), 1.4, 0.0001);
    ASSERT_TRUE(update("two", {7}, {}).isEmpty());
    EXPECT_NEAR(strength(), 0.7, 0.0001);
    controls.clearOwner("one");
    EXPECT_NEAR(strength(), 0.4, 0.0001);
    ASSERT_TRUE(update("one", {7, 8}, {{"strength", 0.7}}, 0, "tab").isEmpty());
    controls.removePane(7);
    EXPECT_NEAR(strength(), 0.4, 0.0001);
    EXPECT_NEAR(strength(8), 0.7, 0.0001);
}
TEST_F(ShaderControlsTest, WindowEffectsAreIndependentOfFocus)
{
    EXPECT_FALSE(update("one", {7}, {{"strength", 1.}}, 2).isEmpty());
    ASSERT_TRUE(update("one", {7, 8}, {{"strength", 1.}}, 2, "window").isEmpty());
    EXPECT_NEAR(strength(7, 2), 1., 0.0001);
    EXPECT_NEAR(strength(9, 2), 1., 0.0001);
    EXPECT_NEAR(strength(9, 0), 0.4, 0.0001);
    controls.removePane(7);
    EXPECT_NEAR(strength(8, 2), 1., 0.0001);
}
TEST_F(ShaderControlsTest, RebuildExpiresIdsAndOverrides)
{
    const auto old = id();
    ASSERT_TRUE(update("one", {7}, {{"strength", 1.}}).isEmpty());
    controls.rebuild(chains);
    EXPECT_NE(id(), old);
    EXPECT_FALSE(controls.update("one", {7}, old, "pane", {{"strength", 1.}}).isEmpty());
    EXPECT_NEAR(strength(), 0.4, 0.0001);
}
TEST_F(ShaderControlsTest, SparseUniformPackingPreservesOtherSlots)
{
    ShaderParameterValues packed;
    ASSERT_TRUE(library.validateParameters("bloom", {{"radius", 12.}, {"strength", 0.8}}, packed).isEmpty());
    TerminalShaderUniforms uniforms{};
    uniforms.customParams[7][3] = 19;
    uniforms.customColors[15][3] = 0.7;
    packed.apply(uniforms);
    EXPECT_FLOAT_EQ(uniforms.customParams[0][0], 12);
    EXPECT_FLOAT_EQ(uniforms.customParams[0][1], 0.8);
    EXPECT_FLOAT_EQ(uniforms.customParams[7][3], 19);
    EXPECT_FLOAT_EQ(uniforms.customColors[15][3], 0.7);
}
TEST_F(ShaderControlsTest, ColorsBooleansAndIntegersUseTypedValidation)
{
    ShaderParameterValues packed;
    EXPECT_TRUE(library.validateParameters("types", {{"count", 4.}, {"enabled", true}, {"tint", "#80402010"}}, packed).isEmpty());
    EXPECT_EQ(packed.paramMask, 3u);
    EXPECT_EQ(packed.colorMask, 1u);
    EXPECT_FLOAT_EQ(packed.params[0], 4.);
    EXPECT_FLOAT_EQ(packed.params[1], 1.);
    EXPECT_NEAR(packed.colors[3], 128. / 255., 0.0001);
    for (const auto& bad : {QVariantMap{{"count", 4.5}}, QVariantMap{{"count", 1e20}},
         QVariantMap{{"enabled", 1.}}, QVariantMap{{"tint", "invalid"}}})
        EXPECT_FALSE(library.validateParameters("types", bad, packed).isEmpty());
    EXPECT_FLOAT_EQ(packed.params[0], 4.); // invalid requests leave packed output intact
}
TEST_F(ShaderControlsTest, PresetsReusePhosphorStoreAndSurviveRestart)
{
    QVariantMap values, result;
    EXPECT_TRUE(library.presetParameters("types", "One", values).isEmpty());
    EXPECT_EQ(values.value("count").toInt(), 1);
    ASSERT_TRUE(library.savePreset("types", "Test saved scene", {{"count", 5}, {"enabled", true}, {"tint", "#ff7700"}}, result).isEmpty());
    const auto savedId = result.value("id").toString();
    ShaderLibrary restarted;
    EXPECT_TRUE(restarted.presetParameters("types", savedId, values).isEmpty());
    EXPECT_EQ(values.value("count").toInt(), 5);
    EXPECT_TRUE(values.value("enabled").toBool());
    EXPECT_TRUE(restarted.presetParameters("types", "@defaults", values).isEmpty());
    EXPECT_EQ(values.value("count").toInt(), 2);
    EXPECT_FALSE(restarted.presetParameters("types", "does-not-exist", values).isEmpty());
    EXPECT_FALSE(library.savePreset("types", "", {{"count", 5}}, result).isEmpty());
    EXPECT_FALSE(library.savePreset("types", "Invalid", {{"count", 99}}, result).isEmpty());
}
class ShaderClock : public PhosphorAnimation::IMotionClock
{
public:
    std::chrono::nanoseconds time{};
    std::chrono::nanoseconds now() const override { return time; }
    qreal refreshRate() const override { return 60; }
    void requestFrame() override {}
    const void* epochIdentity() const override { return steadyClockEpoch(); }
    void step(int ms) { time += std::chrono::milliseconds(ms); }
};
TEST_F(ShaderControlsTest, TransitionsRetargetFromVisibleValueAndStopOnCleanup)
{
    ShaderClock clock;
    controls.setClock(&clock);
    const QVariantMap transition{{"duration_ms", 1000.}, {"curve", "linear"}};
    ASSERT_TRUE(controls.update("one", {7,8}, id(), "tab", {{"strength", 1.4}}, transition).isEmpty());
    EXPECT_NEAR(strength(), 0.4, 0.001);
    clock.step(500); EXPECT_TRUE(controls.advance());
    EXPECT_NEAR(strength(), 0.9, 0.001); EXPECT_NEAR(strength(8), 0.9, 0.001);
    ASSERT_TRUE(controls.update("one", {7}, id(), "pane", {{"strength", 0.4}}, transition).isEmpty());
    EXPECT_NEAR(strength(), 0.9, 0.001);
    clock.step(500); controls.advance();
    EXPECT_NEAR(strength(), 0.65, 0.001); EXPECT_NEAR(strength(8), 1.4, 0.001);
    controls.clearOwner("one"); EXPECT_FALSE(controls.advance()); EXPECT_NEAR(strength(), 0.4, 0.001);
}
TEST_F(ShaderControlsTest, InvalidTransitionsAreAtomicAndSpringHasBoundedLifetime)
{
    ShaderClock clock; controls.setClock(&clock);
    for (const QVariantMap& bad : {QVariantMap{{"duration_ms", -1.}}, QVariantMap{{"duration_ms", "100"}},
        QVariantMap{{"duration_ms", 60001.}}, QVariantMap{{"curve", "missing"}}, QVariantMap{{"typo", 1}}})
        EXPECT_FALSE(controls.update("one", {7}, id(), "pane", {{"strength", 1.4}}, bad).isEmpty());
    EXPECT_NEAR(strength(), 0.4, 0.001);
    ASSERT_TRUE(controls.update("one", {7}, id(), "pane", {{"strength", 1.4}}, {{"duration_ms", 300.}, {"curve", "spring"}}).isEmpty());
    for (int i=0;i<40;++i) { clock.step(10); controls.advance(); }
    EXPECT_FALSE(controls.advance()); EXPECT_NEAR(strength(), 1.4, 0.001);
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    const auto path = config.filePath(QStringLiteral("afterglow/shaders/types"));
    QDir().mkpath(path);
    QFile metadata(path + QStringLiteral("/metadata.json"));
    if (!metadata.open(QIODevice::WriteOnly)) return 1;
    metadata.write(R"({"id":"types","name":"Types","fragmentShader":"effect.frag","presets":{"One":{"count":1}},"parameters":[
        {"id":"count","name":"Count","type":"int","default":2,"min":1,"max":8},
        {"id":"enabled","name":"Enabled","type":"bool","default":false},
        {"id":"tint","name":"Tint","type":"color","default":"#ffffff"}]})");
    metadata.close();
    QFile shader(path + QStringLiteral("/effect.frag"));
    if (!shader.open(QIODevice::WriteOnly)) return 1;
    shader.write("void mainImage(out vec4 c, in vec2 p) { c = vec4(1.0); }");
    shader.close();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
