// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Application.h"
#include "scripting/ScriptHost.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QQuickWindow>
#include <QSaveFile>
#include <QScreen>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>

using namespace Afterglow;
namespace {
double nowMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
double cpuMs()
{
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
void save(const QString& path, const QJsonObject& data)
{
    QSaveFile f(path);
    const QByteArray bytes = QJsonDocument(data).toJson();
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        throw std::runtime_error("Cannot save benchmark results");
}
void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
QJsonObject distribution(const std::vector<double>& values)
{
    if (values.empty()) return {{QStringLiteral("count"), 0}};
    auto ordered = values;
    std::sort(ordered.begin(), ordered.end());
    auto percentile = [&](double p) { return ordered[std::min(ordered.size()-1, std::size_t(std::ceil(p*ordered.size())-1))]; };
    double sum = 0;
    QJsonArray samples;
    for (double v : values) { sum += v; samples.append(v); }
    return {{QStringLiteral("count"), int(values.size())}, {QStringLiteral("mean_ms"), sum/values.size()},
            {QStringLiteral("p50_ms"), percentile(.5)}, {QStringLiteral("p95_ms"), percentile(.95)},
            {QStringLiteral("p99_ms"), percentile(.99)}, {QStringLiteral("max_ms"), ordered.back()},
            {QStringLiteral("samples_ms"), samples}};
}
QJsonObject memory()
{
    QJsonObject result;
    QFile f(QStringLiteral("/proc/self/smaps_rollup"));
    if (f.open(QIODevice::ReadOnly)) {
        for (const auto& line : f.readAll().split('\n')) {
            const auto parts = line.simplified().split(' ');
            if (parts.size() >= 2 && (parts[0] == "Rss:" || parts[0] == "Pss:" || parts[0] == "Private_Dirty:"))
                result.insert(QString::fromLatin1(parts[0].chopped(1)) + QStringLiteral("_kib"), parts[1].toDouble());
        }
    }
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    result.insert(QStringLiteral("peak_rss_kib"), double(usage.ru_maxrss));
    return result;
}
class BenchApplication : public QApplication
{
public:
    using QApplication::QApplication;
    TerminalWindow* target = nullptr;
    bool measuring = false;
    std::vector<double> frames, intervals, gpu;
    std::function<void()> afterFrame;
    double previous = 0;
    QJsonArray frameSizes;
    bool notify(QObject* receiver, QEvent* event) override
    {
        if (!measuring || receiver != target || (event->type() != QEvent::UpdateRequest && event->type() != QEvent::Expose))
            return QApplication::notify(receiver, event);
        const auto before = target->submittedFrames();
        const double start = nowMs();
        const bool result = QApplication::notify(receiver, event);
        if (target->submittedFrames() == before) return result;
        frames.push_back(nowMs() - start);
        const QJsonArray size{target->width(), target->height()};
        if (frameSizes.isEmpty() || frameSizes.last() != size) frameSizes.append(size);
        const double elapsedGpu = target->completedGpuSeconds()*1000;
        if (elapsedGpu > 0 && std::isfinite(elapsedGpu)) gpu.push_back(elapsedGpu);
        if (previous != 0) intervals.push_back(start - previous);
        previous = start;
        if (afterFrame) afterFrame();
        return result;
    }
};
void fill(Pane* pane, int history)
{
    const std::string row = "0123456789 abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789\r\n";
    const int count = history + pane->rows() - 1;
    pane->withTerminal([&](AfterglowVt::Terminal& terminal) {
        std::string batch;
        for (int i=0; i<1000; ++i) batch += row;
        for (int i=0; i<count; i+=1000)
            terminal.feed(std::string_view(batch).substr(0, std::size_t(std::min(1000,count-i))*row.size()));
    });
}
void key(TerminalWindow* window, const QString& text)
{
    const int code = text == QStringLiteral("\r") ? int(Qt::Key_Return) : int(text.front().toUpper().unicode());
    QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier, text);
    QCoreApplication::sendEvent(window, &release);
}
}
int main(int argc, char** argv)
{
    BenchApplication qt(argc, argv);
    qt.setQuitOnLastWindowClosed(false);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native terminal performance workloads; timings exclude physical display latency."));
    parser.addHelpOption();
    for (const auto& name : {"case", "output", "config", "child", "repo", "width", "height", "duration", "history", "samples", "panes"})
        parser.addOption(QCommandLineOption(QString::fromLatin1(name), QString::fromLatin1(name), QStringLiteral("value")));
    parser.process(qt);
    auto str = [&](const char* name) { return parser.value(QString::fromLatin1(name)); };
    auto integer = [&](const char* name, int fallback, int min, int max) {
        if (!parser.isSet(QString::fromLatin1(name))) return fallback;
        bool ok = false; const int v = str(name).toInt(&ok);
        if (!ok || v < min || v > max) throw std::runtime_error(std::string("Invalid ")+name);
        return v;
    };
    try {
        const QString mode = str("case"), out = str("output");
        const QStringList modes{QStringLiteral("idle"), QStringLiteral("idle-blink"), QStringLiteral("scroll"),
            QStringLiteral("resize"), QStringLiteral("history"), QStringLiteral("input"), QStringLiteral("input-load"), QStringLiteral("scripts")};
        if (!modes.contains(mode) || out.isEmpty() || str("config").isEmpty()) throw std::runtime_error("Missing or invalid case/output/config");
        const int width = integer("width",1920,640,7680), height = integer("height",1080,480,4320);
        const int duration = integer("duration",15,1,120), retained = integer("history",100000,0,1000000);
        const int sampleCount = integer("samples",100,1,1000), paneCount = integer("panes",8,1,128);
        QJsonObject result{{QStringLiteral("case"),mode}, {QStringLiteral("pid"),double(getpid())},
            {QStringLiteral("qt"),QString::fromLatin1(qVersion())}, {QStringLiteral("duration_requested_s"),duration}};
        QJsonObject settings{{QStringLiteral("Terminal"),QJsonObject{{QStringLiteral("cursorBlink"),mode==QLatin1String("idle-blink")},
            {QStringLiteral("scrollback"),1000000}, {QStringLiteral("shellIntegration"),false}}},
            {QStringLiteral("Window"),QJsonObject{{QStringLiteral("decorations"),false}, {QStringLiteral("tabBar"),QStringLiteral("never")}}},
            {QStringLiteral("Audio"),QJsonObject{{QStringLiteral("enabled"),false}}},
            {QStringLiteral("Scripts"),QJsonObject{{QStringLiteral("enabled"),false}}}};
        Application app(str("config"),settings);
        struct ResetObserver {
            BenchApplication& qt;
            ~ResetObserver() { qt.measuring = false; qt.target = nullptr; qt.afterFrame = {}; }
        } resetObserver{qt};
        WindowOptions options;
        options.command = {QStringLiteral("/bin/cat")};
        if (mode.startsWith(QLatin1String("input"))) {
            if (str("child").isEmpty()) throw std::runtime_error("Input case needs --child");
            options.command = {QStringLiteral("python3"),str("child"),mode,out};
        }
        auto* window = app.newWindow(options);
        qt.target = window;
        window->setGpuTimingEnabled(true);
        window->resize(width,height);
        window->show();
        waitMs(2000);
        if (!window->isExposed()) throw std::runtime_error("Benchmark window is not exposed");
        if (window->size()!=QSize(width,height)) throw std::runtime_error(QStringLiteral("Compositor changed %1x%2 to %3x%4").arg(width).arg(height).arg(window->width()).arg(window->height()).toStdString());
        auto* pane = window->focusedPane();
        if (!pane || pane->isHeld()) throw std::runtime_error("Child did not start");
        result.insert(QStringLiteral("width"),window->width());
        result.insert(QStringLiteral("height"),window->height());
        result.insert(QStringLiteral("dpr"),window->devicePixelRatio());
        result.insert(QStringLiteral("refresh_hz"),window->screen()->refreshRate());
        result.insert(QStringLiteral("cols"),pane->cols()); result.insert(QStringLiteral("rows"),pane->rows());
        if (mode.startsWith(QLatin1String("idle"))) {
            fill(pane,0); window->requestUpdate(); waitMs(1000);
        }
        result.insert(QStringLiteral("memory_before"),memory());
        if (mode==QLatin1String("history") || mode==QLatin1String("scroll") || mode==QLatin1String("resize")) {
            const double begin = nowMs(); fill(pane,retained);
            result.insert(QStringLiteral("history_fill_ms"),nowMs()-begin);
            window->requestUpdate(); waitMs(1000);
            const auto actual = pane->withTerminal([](auto& t) { return t.screen().historySize(); });
            if (actual != std::size_t(retained)) throw std::runtime_error("History population did not match requested rows");
            result.insert(QStringLiteral("history_rows"),double(actual));
        }
        save(out+QStringLiteral(".phase.json"),{{QStringLiteral("phase"),QStringLiteral("measure")},{QStringLiteral("pid"),double(getpid())}});
        const double wallStart = nowMs(), cpuStart = cpuMs();
        qt.measuring = true;
        QTimer driver;
        driver.setTimerType(Qt::PreciseTimer);
        int operations = 0;
        std::vector<double> operationTimes, inputTimes;
        int received = 0;
        if (mode==QLatin1String("scroll")) {
            QObject::connect(&driver,&QTimer::timeout,[&] {
                const double start=nowMs();
                // Repeated scrollbar traversal, including near-oldest and newest rows.
                pane->scrollHistory(double((operations*37)%1000)/1000.0);
                window->requestUpdate(); ++operations; operationTimes.push_back(nowMs()-start);
            });
            driver.start(16); waitMs(duration*1000);
        } else if (mode==QLatin1String("resize")) {
            QObject::connect(&driver,&QTimer::timeout,[&] {
                const double start=nowMs();
                const int step=operations%20, delta=(step<=10?step:20-step)*16;
                window->resize(width-delta,height-delta/2);
                ++operations; operationTimes.push_back(nowMs()-start);
            });
            driver.start(50); waitMs(duration*1000);
        } else if (mode.startsWith(QLatin1String("input"))) {
            QString pending;
            double sent=0;
            QEventLoop loop;
            QTimer deadline;
            deadline.setSingleShot(true);
            QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
            qt.afterFrame=[&] {
                if (pending.isEmpty()) return;
                const auto& snapshot=pane->snapshot();
                QString first;
                for (int i=0;i<snapshot.cols && i<int(snapshot.cells.size());++i) first+=QChar(ushort(snapshot.cells[i].cp));
                if (first.contains(pending)) {
                    inputTimes.push_back(nowMs()-sent);pending.clear();++received;
                    if (received==sampleCount) loop.quit();
                }
            };
            QObject::connect(&driver,&QTimer::timeout,[&] {
                if (!pending.isEmpty() || operations>=sampleCount) return;
                pending=QStringLiteral("ACK%1").arg(++operations,6,10,QLatin1Char('0'));
                sent=nowMs();
                for (QChar ch : pending) key(window,QString(ch));
                key(window,QStringLiteral("\r"));
            });
            driver.start(100);deadline.start(sampleCount*100+30000);loop.exec();
            qt.afterFrame={};
            if (received!=sampleCount) throw std::runtime_error("Input acknowledgements timed out");
            result.insert(QStringLiteral("input_to_submitted_snapshot"),distribution(inputTimes));
            result.insert(QStringLiteral("output_target_bytes_per_second"),mode==QLatin1String("input-load")?4194304:0);
        } else if (mode==QLatin1String("scripts")) {
            const QString scriptDir=QFileInfo(str("config")).absolutePath()+QStringLiteral("/benchmark-scripts");
            QDir().mkpath(scriptDir);
            QFile source(str("repo")+QStringLiteral("/data/examples/phosphor/scripts/phosphor.luau"));
            if (!source.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read Phosphor script");
            QByteArray scriptSource = source.readAll();
            const QByteArray marker("return script");
            const auto index = scriptSource.lastIndexOf(marker);
            if (index < 0) throw std::runtime_error("Missing script export");
            scriptSource.insert(index, "local measured = script.on_command_finished\n"
                "script.on_command_finished = function(ev) measured(ev); return nil end\n");
            QFile dest(scriptDir+QStringLiteral("/phosphor.luau"));
            if (!dest.open(QIODevice::WriteOnly) || dest.write(scriptSource)!=scriptSource.size()) throw std::runtime_error("Cannot copy Phosphor script");
            dest.close();
            QVariantList panes;
            for (int i=1;i<=paneCount;++i) panes << QVariantMap{{QStringLiteral("id"),i},{QStringLiteral("tab_id"),1},
                {QStringLiteral("window"),1},{QStringLiteral("command"),QStringLiteral("")}};
            const QVariantMap state{{QStringLiteral("version"),1},{QStringLiteral("panes"),panes},
                {QStringLiteral("active_pane"),1},{QStringLiteral("active_tab"),1},{QStringLiteral("active_window"),1}};
            ScriptHost::Callbacks callbacks;
            callbacks.state=[&] { return state; };
            callbacks.paneInfo=[&](quint32 id) { return panes.value(int(id)-1).toMap(); };
            callbacks.apply=[](const QVariantMap&) { return ScriptHost::EffectResult{}; };
            ScriptHost host(callbacks); host.setSearchPaths({scriptDir});host.setEnabled(true);
            if (host.scriptIds()!=QStringList{QStringLiteral("phosphor")}) throw std::runtime_error("Phosphor script did not load");
            // Execute the real callback, but discard returned effects to isolate VM time.
            const int calls=10000;
            for (int i=0;i<calls;++i) {
                host.dispatch(QStringLiteral("on_command_finished"),1,
                    {{QStringLiteral("exit_code"),0},{QStringLiteral("duration_ms"),2000}});
                const auto diag=host.diagnostics().first().toMap();
                if (diag.value(QStringLiteral("handler_errors")).toULongLong()!=0 || host.isDisabled(QStringLiteral("phosphor")))
                    throw std::runtime_error("Script callback failed");
                operationTimes.push_back(diag.value(QStringLiteral("last_call_ms")).toDouble());
            }
            result.insert(QStringLiteral("script_diagnostics"),QJsonArray::fromVariantList(host.diagnostics()));
            result.insert(QStringLiteral("snapshot_panes"),paneCount);
            operations=calls;
        } else waitMs(duration*1000);
        driver.stop();qt.measuring=false;
        const double wall=nowMs()-wallStart,cpu=cpuMs()-cpuStart;
        result.insert(QStringLiteral("wall_ms"),wall);result.insert(QStringLiteral("cpu_ms"),cpu);
        result.insert(QStringLiteral("cpu_percent_one_core"),100*cpu/wall);
        result.insert(QStringLiteral("frame_service"),distribution(qt.frames));
        result.insert(QStringLiteral("terminal_gpu_elapsed"),distribution(qt.gpu));
        result.insert(QStringLiteral("frame_intervals"),distribution(qt.intervals));
        result.insert(QStringLiteral("submitted_frame_sizes"),qt.frameSizes);
        result.insert(QStringLiteral("operation"),distribution(operationTimes));
        result.insert(QStringLiteral("operations"),operations);
        result.insert(QStringLiteral("memory_after"),memory());
        save(out+QStringLiteral(".phase.json"),{{QStringLiteral("phase"),QStringLiteral("done")},{QStringLiteral("pid"),double(getpid())}});
        save(out,result);
        qt.target=nullptr;
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
