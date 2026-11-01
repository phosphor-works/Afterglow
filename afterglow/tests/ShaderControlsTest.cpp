// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaders/ShaderControls.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QThread>
#include <QSet>
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

namespace {
/// The base packs main() copied from data/: every bundled and example pack.
QSet<QString> copiedPacks;
}

TEST_F(ShaderControlsTest, EveryBundledAndExamplePackCompiles)
{
    const auto ids = library.packIds();
    // A pack whose metadata stops parsing drops out of packIds(); a count would miss it.
    ASSERT_FALSE(copiedPacks.isEmpty());
    for (const auto& pack : std::as_const(copiedPacks)) EXPECT_TRUE(ids.contains(pack)) << pack.toStdString();
    for (const auto& pack : ids) {
        SCOPED_TRACE(pack.toStdString());
        ShaderLibrary::Refs refs;
        refs.background = {pack};
        const auto result = library.build(refs, {});
        ASSERT_EQ(result.background.size(), 1);
        EXPECT_TRUE(result.background.front()->shader().isValid());
    }
}

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
TEST_F(ShaderControlsTest, ConfiguredValuesApplyWithoutCompilingAgain)
{
    ShaderLibrary::Refs refs;
    refs.window = {QStringLiteral("feedback-glow")};
    const auto low = library.build(refs, {{QStringLiteral("feedback-glow"), QVariantMap{{QStringLiteral("strength"), 0.1}}}});
    const auto high = library.build(refs, {{QStringLiteral("feedback-glow"), QVariantMap{{QStringLiteral("strength"), 0.4}}}});
    const auto defaults = library.build(refs, {});
    ASSERT_EQ(low.window.size(), 1u);
    ASSERT_EQ(high.window.size(), 1u);
    ASSERT_EQ(defaults.window.size(), 1u);
    ASSERT_FALSE(low.window[0]->buffers().empty());
    // One compile for the pack: each configuration is a copy that shares its passes.
    EXPECT_EQ(low.window[0]->buffers()[0], high.window[0]->buffers()[0]);
    EXPECT_EQ(low.window[0]->buffers()[0], defaults.window[0]->buffers()[0]);
    int slot = -1;
    for (const auto& parameter : library.parameters(QStringLiteral("feedback-glow")))
        if (parameter.toMap().value(QStringLiteral("id")) == QStringLiteral("strength"))
            slot = parameter.toMap().value(QStringLiteral("slot")).toInt();
    ASSERT_GE(slot, 0);
    EXPECT_FLOAT_EQ(low.window[0]->params()[std::size_t(slot)], 0.1f);
    EXPECT_FLOAT_EQ(high.window[0]->params()[std::size_t(slot)], 0.4f);
    EXPECT_FLOAT_EQ(defaults.window[0]->params()[std::size_t(slot)], 0.15f);
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
    PhosphorShaders::BaseUniforms uniforms{};
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
    // The developer's ~/.local/share/afterglow/shaders and the system's stay out.
    qputenv("XDG_DATA_HOME", config.filePath(QStringLiteral("data-home")).toUtf8());
    qputenv("XDG_DATA_DIRS", config.filePath(QStringLiteral("data-dirs")).toUtf8());
    const QString packages = QStringLiteral(AFTERGLOW_SHADER_DATA_DIR "/examples/packages/");
    QDirIterator packs(QStringLiteral(AFTERGLOW_SHADER_DATA_DIR), {QStringLiteral("metadata.json")}, QDir::Files, QDirIterator::Subdirectories);
    while (packs.hasNext()) {
        const QFileInfo metadata(packs.next());
        // Package packs load only through their package, as "<package>.<pack>".
        if (metadata.absoluteFilePath().startsWith(packages)) continue;
        const QDir source = metadata.dir();
        copiedPacks.insert(source.dirName());
        const QString target = config.filePath(QStringLiteral("afterglow/shaders/") + source.dirName());
        if (!QDir().mkpath(target)) return 1;
        QDirIterator files(source.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString from = files.next();
            const QString to = target + QLatin1Char('/') + source.relativeFilePath(from);
            if (!QDir().mkpath(QFileInfo(to).absolutePath()) || !QFile::copy(from, to)) return 1;
        }
    }
    const QString shared = config.filePath(QStringLiteral("afterglow/shaders/shared"));
    if (!QDir().mkpath(shared)) return 1;
    QDirIterator includes(QStringLiteral(AFTERGLOW_SHADER_DATA_DIR), {QStringLiteral("*.glsl")}, QDir::Files, QDirIterator::Subdirectories);
    while (includes.hasNext()) {
        const QString from = includes.next();
        if (QFileInfo(from).dir().dirName() == QLatin1String("shared")
            && !QFile::copy(from, shared + QLatin1Char('/') + QFileInfo(from).fileName())) return 1;
    }
    const auto path = config.filePath(QStringLiteral("afterglow/shaders/types"));
    QDir().mkpath(path);
    QFile metadata(path + QStringLiteral("/metadata.json"));
    if (!metadata.open(QIODevice::WriteOnly)) return 1;
    metadata.write(R"({"id":"types","name":"Types","fragmentShader":"effect.frag","presets":{"One":{"count":1}},"parameters":[
        {"id":"count","name":"Count","type":"int","default":2,"min":1,"max":8},
        {"id":"enabled","name":"Enabled","type":"bool","default":false},
        {"id":"tint","name":"Tint","type":"color","default":"#ffffff"},
        {"id":"photo","name":"Photo","type":"image","default":"red.png"}]})");
    metadata.close();
    QFile shader(path + QStringLiteral("/effect.frag"));
    if (!shader.open(QIODevice::WriteOnly)) return 1;
    shader.write("vec4 pTerminal(vec2 uv) { return vec4(1.0); }");
    shader.close();
    QImage red(2,2,QImage::Format_RGBA8888); red.fill(Qt::red);
    QImage blue(2,2,QImage::Format_RGBA8888); blue.fill(Qt::blue);
    if (!red.save(path+QStringLiteral("/red.png")) || !blue.save(path+QStringLiteral("/blue.png"))) return 1;
    const auto passes=config.filePath(QStringLiteral("afterglow/shaders/passes"));
    QDir().mkpath(passes);
    QFile multi(passes+QStringLiteral("/metadata.json"));
    if(!multi.open(QIODevice::WriteOnly)) return 1;
    multi.write(R"({"id":"passes","name":"Passes","multipass":true,"fragmentShader":"effect.frag","bufferShaders":["a.frag","b.frag"],"bufferWraps":["repeat","mirror"],"bufferFilters":["nearest","linear"],"bufferFeedback":true})"); multi.close();
    for(const auto& name:{QStringLiteral("effect.frag"),QStringLiteral("a.frag"),QStringLiteral("b.frag")}) {
        QFile f(passes+QLatin1Char('/')+name); if(!f.open(QIODevice::WriteOnly)) return 1;
        f.write("#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}");
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST_F(ShaderControlsTest, ImagesLoadTransactionallyAndTransitionAtCompletion)
{
    ShaderLibrary::Refs refs; refs.background={QStringLiteral("types")};
    auto images=library.build(refs,{}); ASSERT_EQ(images.background.size(),1u);
    controls.rebuild(images); ShaderClock clock; controls.setClock(&clock);
    const auto instance=controls.describe(7).front().toMap()["instance"].toString();
    ShaderParameterValues packed;
    ASSERT_TRUE(library.validateParameters("types",{{"photo","blue.png"}},packed).isEmpty());
    ASSERT_EQ(packed.imageMask,1); EXPECT_EQ(packed.images[0].pixelColor(0,0),QColor(Qt::blue));
    EXPECT_FALSE(library.validateParameters("types",{{"photo","missing.png"}},packed).isEmpty());
    EXPECT_EQ(packed.images[0].pixelColor(0,0),QColor(Qt::blue));
    ASSERT_TRUE(controls.update("test",{7},instance,"pane",{{"photo","blue.png"}},{{"duration_ms",1000.}}).isEmpty());
    clock.step(500); controls.advance();
    EXPECT_EQ(controls.values(7)[instance].images[0].pixelColor(0,0),QColor(Qt::red));
    clock.step(600); controls.advance();
    EXPECT_EQ(controls.values(7)[instance].images[0].pixelColor(0,0),QColor(Qt::blue));
    QVariantMap saved, values;
    ASSERT_TRUE(library.savePreset("types","Image scene",{{"photo","blue.png"}},saved).isEmpty());
    EXPECT_TRUE(library.presetParameters("types",saved["id"].toString(),values).isEmpty());
    EXPECT_EQ(values["photo"].toString(),"blue.png");
}
TEST_F(ShaderControlsTest, MultipleBufferMetadataPreservesSamplerModes)
{
    ShaderLibrary::Refs refs; refs.background={QStringLiteral("passes")};
    const auto result=library.build(refs,{}); ASSERT_EQ(result.background.size(),1u);
    const auto& program=*result.background.front(); ASSERT_EQ(program.buffers().size(),2u);
    EXPECT_EQ(program.bufferSettings(0).wrap,ShaderWrap::Repeat);
    EXPECT_EQ(program.bufferSettings(1).wrap,ShaderWrap::Mirror);
    EXPECT_TRUE(program.bufferSettings(0).nearest); EXPECT_FALSE(program.bufferSettings(1).nearest);
}

TEST_F(ShaderControlsTest, DecodedImageBudgetRejectsWholeUpdate)
{
    // Inside the pack: scripts reach only the pack's own files.
    QImage image(4096, 1024, QImage::Format_RGBA8888);
    image.fill(Qt::green);
    const QString path = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/afterglow/shaders/types/large.png");
    ASSERT_TRUE(image.save(path));
    ShaderLibrary::Refs refs; refs.background = {QStringLiteral("types")};
    auto chain = library.build(refs, {}); ASSERT_EQ(chain.background.size(), 1u);
    controls.rebuild(chain);
    const auto instance = controls.describe(7).front().toMap()["instance"].toString();
    // Distinct decodes stay retained under different owners. Including the
    // configured tiny default, the sixteenth 16-MiB image exceeds the budget.
    for (int i = 0; i < 15; ++i)
        ASSERT_TRUE(controls.update(QString::number(i), {7}, instance, "pane", {{"photo", path}}).isEmpty());
    const auto previous = controls.values(7)[instance].images[0].cacheKey();
    EXPECT_FALSE(controls.update("overflow", {7}, instance, "pane", {{"photo", path}}).isEmpty());
    EXPECT_EQ(controls.values(7)[instance].images[0].cacheKey(), previous);
    EXPECT_TRUE(controls.update("0", {7}, instance, "pane", {}).isEmpty());
    EXPECT_TRUE(controls.update("overflow", {7}, instance, "pane", {{"photo", path}}).isEmpty());
    for (int i = 0; i < 15; ++i) controls.clearOwner(QString::number(i));
    controls.clearOwner("overflow");
    ASSERT_TRUE(QFile::remove(path));
}

TEST_F(ShaderControlsTest, ScriptImagesStayInsideThePack)
{
    QTemporaryDir outside;
    QImage image(2, 2, QImage::Format_RGBA8888);
    image.fill(Qt::green);
    const QString path = outside.filePath(QStringLiteral("private.png"));
    ASSERT_TRUE(image.save(path));
    ShaderParameterValues packed;
    EXPECT_FALSE(library.validateParameters("types", {{"photo", path}}, packed).isEmpty());
    EXPECT_FALSE(library.validateParameters("types", {{"photo", "../bloom/../types/../../x.png"}}, packed).isEmpty());
    EXPECT_FALSE(library.validateParameters("types", {{"photo", "~/private.png"}}, packed).isEmpty());
    // An absolute path inside the pack is still the pack's file.
    const QString inside = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/afterglow/shaders/types/blue.png");
    EXPECT_TRUE(library.validateParameters("types", {{"photo", inside}}, packed).isEmpty());
    // The user's own config may still name any image.
    ShaderLibrary::Refs refs; refs.background = {QStringLiteral("types")};
    const auto configured = library.build(refs, {{QStringLiteral("types"), QVariantMap{{QStringLiteral("photo"), path}}}});
    ASSERT_EQ(configured.background.size(), 1u);
    EXPECT_EQ(configured.background.front()->images()[0].pixelColor(0, 0), QColor(Qt::green));
}

TEST_F(ShaderControlsTest, ConfiguredImageRescuesAPackWhoseDefaultImageFails)
{
    const auto dir = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/afterglow/shaders/missing-default");
    ASSERT_TRUE(QDir().mkpath(dir));
    QFile metadata(dir + QStringLiteral("/metadata.json"));
    ASSERT_TRUE(metadata.open(QIODevice::WriteOnly));
    metadata.write(R"({"id":"missing-default","name":"Missing","fragmentShader":"effect.frag","parameters":[
        {"id":"photo","name":"Photo","type":"image","default":"missing.png"}]})");
    metadata.close();
    QFile shader(dir + QStringLiteral("/effect.frag"));
    ASSERT_TRUE(shader.open(QIODevice::WriteOnly));
    shader.write("vec4 pTerminal(vec2 uv) { return vec4(1.0); }");
    shader.close();
    const QString blue = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/afterglow/shaders/types/blue.png");
    // A library made after the pack exists, so its index holds the pack.
    ShaderLibrary fresh;
    ShaderLibrary::Refs refs;
    refs.background = {QStringLiteral("missing-default")};
    // The pack's own image is missing: without a configured one it fails.
    EXPECT_TRUE(fresh.build(refs, {}).background.empty());
    // The config names an image for that slot, and the pack draws with it.
    const auto configured = fresh.build(refs, {{QStringLiteral("missing-default"), QVariantMap{{QStringLiteral("photo"), blue}}}});
    ASSERT_EQ(configured.background.size(), 1u);
    EXPECT_EQ(configured.background.front()->images()[0].pixelColor(0, 0), QColor(Qt::blue));
    // A configured image that fails too still fails the pack.
    EXPECT_TRUE(fresh.build(refs, {{QStringLiteral("missing-default"),
                                      QVariantMap{{QStringLiteral("photo"), dir + QStringLiteral("/absent.png")}}}})
                    .background.empty());
    EXPECT_TRUE(QDir(dir).removeRecursively());
}

TEST_F(ShaderControlsTest, PresetsStoreImagesRelativeToThePack)
{
    const QString inside = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/afterglow/shaders/types/blue.png");
    QVariantMap saved, values;
    ASSERT_TRUE(library.savePreset("types", "Absolute image", {{"photo", inside}}, saved).isEmpty());
    ASSERT_TRUE(library.presetParameters("types", saved["id"].toString(), values).isEmpty());
    EXPECT_EQ(values["photo"].toString(), QStringLiteral("blue.png"));
}

TEST_F(ShaderControlsTest, ConfiguredImageBudgetRecoversAfterRemovingOtherPacks)
{
    const auto base = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/afterglow/shaders/");
    QImage image(4096, 1024, QImage::Format_RGBA8888);
    image.fill(Qt::green);
    ShaderLibrary::Refs refs;
    for (int i = 0; i < 17; ++i) {
        const auto name = QStringLiteral("budget-%1").arg(i);
        const auto dir = base + name;
        ASSERT_TRUE(QDir().mkpath(dir));
        QFile metadata(dir + QStringLiteral("/metadata.json"));
        ASSERT_TRUE(metadata.open(QIODevice::WriteOnly));
        metadata.write((QStringLiteral(R"({"id":"%1","name":"Budget","fragmentShader":"effect.frag","parameters":[{"id":"photo","name":"Photo","type":"image","default":"image.png"}]})").arg(name)).toUtf8());
        metadata.close();
        QFile shader(dir + QStringLiteral("/effect.frag"));
        ASSERT_TRUE(shader.open(QIODevice::WriteOnly));
        shader.write("vec4 pTerminal(vec2 uv){return vec4(1.0);}");
        shader.close();
        ASSERT_TRUE(image.save(dir + QStringLiteral("/image.png")));
        refs.background.push_back(name);
    }
    {
        ShaderLibrary bounded;
        auto full = bounded.build(refs, {});
        EXPECT_EQ(full.background.size(), 16u);
        // Another window builds the same chains: the pack over the budget is
        // not decoded and compiled again only to be dropped.
        static int budgetWarnings = 0;
        budgetWarnings = 0;
        const auto previous = qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
            if (message.contains(QStringLiteral("decoded-image budget"))) ++budgetWarnings;
        });
        EXPECT_EQ(bounded.build(refs, {}).background.size(), 16u);
        qInstallMessageHandler(previous);
        EXPECT_EQ(budgetWarnings, 0);
        full = {};
        refs.background = {QStringLiteral("budget-16")};
        auto recovered = bounded.build(refs, {});
        ASSERT_EQ(recovered.background.size(), 1u);
        EXPECT_EQ(recovered.background.front()->images()[0].pixelColor(0, 0), QColor(Qt::green));
    }
    for (int i = 0; i < 17; ++i)
        EXPECT_TRUE(QDir(base + QStringLiteral("budget-%1").arg(i)).removeRecursively());
}

TEST(ShaderGeometryPacks, LoadsImportedMeshesAndRejectsInvalidMetadata)
{
    const auto base = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/afterglow/shaders/");
    const auto write = [](const QString& path, const QByteArray& bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const std::vector<std::pair<QByteArray,bool>> cases{
        {R"([{"vertexShader":"mesh.vert","mesh":"mesh.obj","instances":2}])",true},
        {R"([{"vertexShader":"mesh.vert","vertexCount":3}])",true},
        {R"([{"vertexShader":"mesh.vert","vertexCount":3.5}])",false},
        {R"([{"vertexShader":"mesh.vert","vertexCount":3,"instances":0}])",false},
        {R"([{"vertexShader":"mesh.vert","mesh":"mesh.obj","vertexCount":3}])",false},
        {R"([{"vertexShader":"mesh.vert","mesh":"../mesh-outside.obj"}])",false},
        {R"([{"vertexShader":"mesh.vert","mesh":"escape.obj"}])",false},
        {R"([{"vertexShader":"mesh.vert","vertexCount":3,"typo":true}])",false},
        {R"([])",false},
        {R"([true])",false},
        {R"([{"vertexShader":"mesh.vert","objects":[{"id":"pivot","spin":[0,"$angle",0]},
            {"id":"body","parent":"pivot","mesh":"mesh.obj","translate":[1,0,0],"scale":2}]}])",true},
        {R"([{"vertexShader":"mesh.vert","objects":[{"id":"body","parent":"missing","mesh":"mesh.obj"}]}])",false},
        {R"([{"vertexShader":"mesh.vert","objects":[{"id":"a","mesh":"mesh.obj"},{"id":"a","mesh":"mesh.obj"}]}])",false},
        {R"([{"vertexShader":"mesh.vert","objects":[{"id":"a","mesh":"mesh.obj","rotate":[0,"$unknown",0]}]}])",false},
        {R"([{"vertexShader":"mesh.vert","objects":[{"id":"a","mesh":"mesh.obj","scale":[1,2]}]}])",false},
        {R"([{"vertexShader":"mesh.vert","instances":2,"objects":[{"id":"a","mesh":"mesh.obj"}]}])",false},
        {R"([{"vertexShader":"mesh.vert","objects":[{"id":"pivot"}]}])",false},
        {R"([{"vertexShader":"mesh.vert","objects":[{"id":"a","mesh":"escape.obj"}]}])",false},
    };
    ASSERT_TRUE(write(base+"mesh-outside.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
    for (std::size_t i=0;i<cases.size();++i) {
        const auto name=QStringLiteral("geometry-test-%1").arg(i);
        const auto dir=base+name;
        ASSERT_TRUE(QDir().mkpath(dir));
        ASSERT_TRUE(write(dir+"/metadata.json", "{\"id\":\""+name.toUtf8()+"\",\"name\":\""+name.toUtf8()
            +"\",\"fragmentShader\":\"effect.frag\",\"multipass\":true,\"bufferShaders\":[\"effect.frag\"],"
            "\"parameters\":[{\"id\":\"angle\",\"name\":\"Angle\",\"type\":\"float\",\"default\":0,\"min\":0,\"max\":360}],"
            "\"geometryPasses\":"+cases[i].first+"}"));
        ASSERT_TRUE(write(dir+"/effect.frag", "vec4 pTerminal(vec2 uv){return vec4(1);}"));
        ASSERT_TRUE(write(dir+"/mesh.vert", "vec4 pMesh(){return vec4(0,0,0,1);}"));
        ASSERT_TRUE(write(dir+"/mesh.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
        ASSERT_TRUE(QFile::link(base+"mesh-outside.obj",dir+"/escape.obj"));
        ShaderLibrary library;
        ShaderLibrary::Refs refs; refs.background={name};
        const auto chains=library.build(refs,{});
        EXPECT_EQ(chains.background.size(),cases[i].second ? 1u : 0u) << i;
        if (i==0 && !chains.background.empty()) {
            const auto& mesh=chains.background[0]->buffers()[0]->geometry();
            EXPECT_EQ(mesh.vertices.size(),3u); EXPECT_EQ(mesh.instances,2);
        }
        if (i==10 && !chains.background.empty()) {
            const auto& scene=chains.background[0]->buffers()[0]->geometry();
            ASSERT_EQ(scene.objects.size(),2u);
            EXPECT_EQ(scene.objects[0].vertexCount,0); EXPECT_EQ(scene.objects[0].spin[1].slot,0);
            EXPECT_EQ(scene.objects[1].parent,0); EXPECT_EQ(scene.objects[1].vertexCount,3);
            EXPECT_FLOAT_EQ(scene.objects[1].scale[2].value,2); EXPECT_TRUE(chains.background[0]->animated());
        }
        // Do not expose intentionally invalid fixtures to the bundled-pack test.
        ASSERT_TRUE(QDir(dir).removeRecursively());
    }
}

namespace {
bool writeShaderFixture(const QString& path, const QByteArray& bytes, bool atomic = false)
{
    if (atomic) {
        QSaveFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool waitForShaderChange(const int& count, int previous, int timeout = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        if (count > previous) return true;
        QThread::msleep(5);
    }
    return false;
}
const QByteArray ShaderBody = "vec4 pTerminal(vec2 uv){return vec4(shade());}\n";
/// A pack in the user's shader directory whose effect.frag is @p fragment.
/// Returns the pack directory.
QString writePack(const QString& name, const QByteArray& fragment)
{
    const auto dir = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/afterglow/shaders/") + name;
    if (!QDir().mkpath(dir)
        || !writeShaderFixture(dir + QStringLiteral("/metadata.json"),
                               "{\"id\":\"" + name.toUtf8() + "\",\"name\":\"" + name.toUtf8() + "\",\"fragmentShader\":\"effect.frag\"}")
        || !writeShaderFixture(dir + QStringLiteral("/effect.frag"), fragment)) return {};
    return dir;
}
}

TEST(ShaderReload, MissingIncludeRecoversThroughNewParentDirectories)
{
    const auto dir = writePack(QStringLiteral("reload-missing"), "#include \"new/deeper/shade.glsl\"\n" + ShaderBody);
    ASSERT_FALSE(dir.isEmpty());
    const auto path = dir + QStringLiteral("/new/deeper/shade.glsl");
    ShaderLibrary library;
    ShaderLibrary::Refs refs; refs.window = {QStringLiteral("reload-missing")};
    int changes = 0;
    QObject::connect(&library, &ShaderLibrary::changed, &library, [&] { ++changes; });
    EXPECT_TRUE(library.build(refs, {}).window.empty());
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    const int before = changes;
    ASSERT_TRUE(writeShaderFixture(path, "float shade(){return 1.;}\n"));
    ASSERT_TRUE(waitForShaderChange(changes, before));
    EXPECT_EQ(library.build(refs, {}).window.size(), 1u);
    ASSERT_TRUE(QDir(dir).removeRecursively());
}

TEST(ShaderReload, MissingNestedIncludesRecoverAndStayWatchedAfterAtomicReplacement)
{
    const auto dir = writePack(QStringLiteral("reload-nested"), "#include \"color.glsl\"\n" + ShaderBody);
    ASSERT_FALSE(dir.isEmpty());
    const auto include = dir + QStringLiteral("/color.glsl");
    const auto nested = dir + QStringLiteral("/nested/tint.glsl");
    ShaderLibrary library;
    ShaderLibrary::Refs refs; refs.window = {QStringLiteral("reload-nested")};
    int changes = 0;
    QObject::connect(&library, &ShaderLibrary::changed, &library, [&] { ++changes; });
    EXPECT_TRUE(library.build(refs, {}).window.empty());
    ASSERT_TRUE(writeShaderFixture(include, "#include \"nested/tint.glsl\"\n"));
    ASSERT_TRUE(waitForShaderChange(changes, 0));
    EXPECT_TRUE(library.build(refs, {}).window.empty());
    int before = changes;
    ASSERT_TRUE(QDir().mkpath(QFileInfo(nested).absolutePath()));
    ASSERT_TRUE(writeShaderFixture(nested, "float shade(){return 0.25;}\n"));
    ASSERT_TRUE(waitForShaderChange(changes, before));
    auto first = library.build(refs, {});
    ASSERT_EQ(first.window.size(), 1u);
    for (bool atomic : {false, true, false}) {
        before = changes;
        ASSERT_TRUE(writeShaderFixture(nested, atomic ? "float shade(){return 0.75;}\n" : "float shade(){return 0.5;}\n", atomic));
        ASSERT_TRUE(waitForShaderChange(changes, before));
        auto rebuilt = library.build(refs, {});
        ASSERT_EQ(rebuilt.window.size(), 1u);
        EXPECT_NE(rebuilt.window.front(), first.window.front());
        first = std::move(rebuilt);
    }
    // Dropping references retires the include watches. (The pack's own stage
    // stays with the registry, which reports edits to any pack.)
    library.build({}, {});
    QCoreApplication::processEvents();
    before = changes;
    ASSERT_TRUE(writeShaderFixture(nested, "float shade(){return 1.;}\n"));
    ASSERT_TRUE(writeShaderFixture(include, "#include \"nested/tint.glsl\"\n// edited\n"));
    // Several debounce periods (100 ms each), so a late signal shows.
    EXPECT_FALSE(waitForShaderChange(changes, before, 500));
    ASSERT_TRUE(QDir(dir).removeRecursively());
}

TEST(ShaderReload, UnrelatedDirectoryWritesDoNotInvalidateCachedPrograms)
{
    const auto dir = writePack(QStringLiteral("reload-unrelated"),
                               "#include \"p_generated.glsl\"\nfloat shade(){return 1.;}\n" + ShaderBody);
    ASSERT_FALSE(dir.isEmpty());
    ShaderLibrary library;
    ShaderLibrary::Refs refs; refs.window = {QStringLiteral("reload-unrelated")};
    auto first = library.build(refs, {});
    ASSERT_EQ(first.window.size(), 1u);
    int changes = 0;
    QObject::connect(&library, &ShaderLibrary::changed, &library, [&] { ++changes; });
    ASSERT_TRUE(writeShaderFixture(dir + QStringLiteral("/p_generated.glsl"), "editor helper"));
    ASSERT_TRUE(writeShaderFixture(dir + QStringLiteral("/unrelated.txt"), "unrelated"));
    EXPECT_FALSE(waitForShaderChange(changes, 0, 500));
    EXPECT_EQ(library.build(refs, {}).window.front(), first.window.front());
    ASSERT_TRUE(QDir(dir).removeRecursively());
}

TEST(ShaderReload, OneEditRebuildsOnceAndOnlyItsPack)
{
    const auto edited = writePack(QStringLiteral("reload-edited"), "float shade(){return 1.;}\n" + ShaderBody);
    const auto other = writePack(QStringLiteral("reload-other"), "float shade(){return 1.;}\n" + ShaderBody);
    ASSERT_FALSE(edited.isEmpty());
    ASSERT_FALSE(other.isEmpty());
    ShaderLibrary library;
    ShaderLibrary::Refs refs; refs.window = {QStringLiteral("reload-edited"), QStringLiteral("reload-other")};
    const auto first = library.build(refs, {});
    ASSERT_EQ(first.window.size(), 2u);
    int changes = 0;
    // A window rebuilds on each signal, as TerminalWindow::applyShaders does.
    AfterglowRender::ShaderChains rebuilt;
    QObject::connect(&library, &ShaderLibrary::changed, &library, [&] { ++changes; rebuilt = library.build(refs, {}); });
    ASSERT_TRUE(writeShaderFixture(edited + QStringLiteral("/effect.frag"), "float shade(){return 0.5;}\n" + ShaderBody));
    ASSERT_TRUE(waitForShaderChange(changes, 0));
    // The registry and the include watch both see the edit; only one rebuilds.
    EXPECT_FALSE(waitForShaderChange(changes, 1, 500));
    ASSERT_EQ(rebuilt.window.size(), 2u);
    EXPECT_NE(rebuilt.window[0], first.window[0]);
    EXPECT_EQ(rebuilt.window[1], first.window[1]);
    ASSERT_TRUE(QDir(edited).removeRecursively());
    ASSERT_TRUE(QDir(other).removeRecursively());
}

TEST(ShaderReload, OversizedStagesAreRefused)
{
    const auto dir = writePack(QStringLiteral("oversized"), ShaderBody + QByteArray(1100 * 1024, '\n'));
    ASSERT_FALSE(dir.isEmpty());
    ShaderLibrary library;
    ShaderLibrary::Refs refs; refs.window = {QStringLiteral("oversized")};
    EXPECT_TRUE(library.build(refs, {}).window.empty());
    ASSERT_TRUE(QDir(dir).removeRecursively());
}

TEST(ShaderLibraryPriority, UserPacksAndIncludesWinOverSystemOnes)
{
    // The system data directory exists only for this library.
    const QString system = qEnvironmentVariable("XDG_DATA_HOME") + QStringLiteral("/afterglow/shaders");
    const QString user = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/afterglow/shaders");
    auto pack = [](const QString& dir, const QByteArray& id, const QByteArray& parameter, const QByteArray& body) {
        return QDir().mkpath(dir)
            && writeShaderFixture(dir + QStringLiteral("/metadata.json"),
                                  "{\"id\":\"" + id + "\",\"name\":\"Priority\",\"fragmentShader\":\"effect.frag\",\"parameters\":"
                                  "[{\"id\":\"" + parameter + "\",\"name\":\"P\",\"type\":\"float\",\"default\":0}]}")
            && writeShaderFixture(dir + QStringLiteral("/effect.frag"), body);
    };
    const QByteArray body = "#include <priority-include.glsl>\n" + ShaderBody;
    ASSERT_TRUE(pack(system + QStringLiteral("/priority"), "priority-system", "system", body));
    ASSERT_TRUE(pack(user + QStringLiteral("/priority"), "priority-user", "user", body));
    ASSERT_TRUE(QDir().mkpath(system + QStringLiteral("/shared")));
    // The system include does not compile, so only the user's lets the pack load.
    ASSERT_TRUE(writeShaderFixture(system + QStringLiteral("/shared/priority-include.glsl"), "this is not GLSL\n"));
    ASSERT_TRUE(writeShaderFixture(user + QStringLiteral("/shared/priority-include.glsl"), "float shade(){return 1.;}\n"));
    {
        ShaderLibrary library;
        for (const QString& ref : {QStringLiteral("priority"), QStringLiteral("PRIORITY")}) {
            const auto parameters = library.parameters(ref);
            ASSERT_EQ(parameters.size(), 1) << ref.toStdString();
            EXPECT_EQ(parameters[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("user")) << ref.toStdString();
        }
        ShaderLibrary::Refs refs; refs.window = {QStringLiteral("priority")};
        EXPECT_EQ(library.build(refs, {}).window.size(), 1u);
    }
    ASSERT_TRUE(QDir(system).removeRecursively());
    ASSERT_TRUE(QDir(user + QStringLiteral("/priority")).removeRecursively());
    ASSERT_TRUE(QFile::remove(user + QStringLiteral("/shared/priority-include.glsl")));
}

TEST(ShaderLibraryPackages, PackagePacksAnswerOnlyToTheirNamespacedId)
{
    ShaderLibrary library;
    const QString package = QStringLiteral(AFTERGLOW_SHADER_DATA_DIR) + QStringLiteral("/examples/packages/scripted-overview");
    auto build = [&](const QString& ref) {
        ShaderLibrary::Refs refs;
        refs.overview = ref;
        return library.build(refs, {}).overview.size();
    };
    EXPECT_EQ(build(QStringLiteral("scripted-overview.wall")), 0u);
    EXPECT_EQ(build(QStringLiteral("wall")), 0u);
    library.setPackageShaders({{QStringLiteral("scripted-overview"), package + QStringLiteral("/shaders")}});
    EXPECT_TRUE(library.packIds().contains(QStringLiteral("scripted-overview.wall")));
    EXPECT_EQ(build(QStringLiteral("scripted-overview.wall")), 1u);
    EXPECT_EQ(build(QStringLiteral("wall")), 0u);
    // Every lookup takes the namespaced id, and only it.
    const QString ref = QStringLiteral("scripted-overview.wall");
    EXPECT_FALSE(library.presets(ref).isEmpty());
    EXPECT_TRUE(library.presets(QStringLiteral("wall")).isEmpty());
    QVariantMap values;
    EXPECT_TRUE(library.presetParameters(ref, QStringLiteral("@defaults"), values).isEmpty());
    EXPECT_FALSE(library.presetParameters(QStringLiteral("wall"), QStringLiteral("@defaults"), values).isEmpty());
    EXPECT_EQ(library.parameters(ref).size(), library.parameters(QStringLiteral("scripted-overview.wall")).size());
    EXPECT_TRUE(library.parameters(QStringLiteral("wall")).isEmpty());
    ShaderParameterValues packed;
    EXPECT_TRUE(library.validateParameters(ref, {}, packed).isEmpty());
    EXPECT_FALSE(library.validateParameters(QStringLiteral("wall"), {}, packed).isEmpty());
    EXPECT_EQ(build(QStringLiteral("pane-deck")), 1u);
    library.setPackageShaders({});
    EXPECT_EQ(build(QStringLiteral("scripted-overview.wall")), 0u);
}
