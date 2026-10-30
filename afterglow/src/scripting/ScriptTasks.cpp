// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScriptTasks.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QProcess>
#include <QtConcurrentRun>
#include <QSemaphore>
#include <atomic>
#include <cmath>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Afterglow {
using Result = ScriptHost::EffectResult;
namespace {
Result failure(const char* code, const QString& text) { return {text, {}, QString::fromLatin1(code)}; }
int bounded(const QVariantMap& effect, const char* key, int fallback, int low, int high)
{
    const double value = effect.value(QLatin1String(key), fallback).toDouble();
    return std::isfinite(value) && value == std::floor(value) && value >= low && value <= high ? int(value) : -1;
}
QSemaphore fileSlots(8);
}
struct ScriptTasks::Task : QObject {
    QString owner, id;
    Completion complete;
    Event event;
    QProcess* process = nullptr;
    QByteArray output, errors;
    bool done = false, truncated = false;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    explicit Task(QObject* parent) : QObject(parent) {}
};
ScriptTasks::~ScriptTasks()
{
    const auto owners = m_tasks.keys();
    for (const auto& owner : owners) clear(owner);
}
void ScriptTasks::finish(Task* task, Result result, bool deliver)
{
    if (task->done) return;
    task->done = true;
    *task->cancelled = true;
    m_tasks[task->owner].remove(task->id);
    if (m_tasks[task->owner].isEmpty()) m_tasks.remove(task->owner);
    if (task->process && task->process->state() != QProcess::NotRunning) {
        const qint64 pid = task->process->processId();
        if (pid > 0) ::kill(-pid_t(pid), SIGKILL);
        task->process->kill();
    }
    result.data.insert(QStringLiteral("name"), task->id);
    auto complete = std::move(task->complete);
    task->deleteLater();
    if (deliver && complete) complete(std::move(result));
}
void ScriptTasks::clear(const QString& owner)
{
    const auto tasks = m_tasks.value(owner).values();
    for (auto* task : tasks) finish(task, {}, false);
}
Result ScriptTasks::apply(const QString& owner, const QVariantMap& e, Completion complete, Event event)
{
    const QString kind = e.value(QStringLiteral("kind")).toString();
    const QString id = e.value(QStringLiteral("name")).toString();
    if (id.isEmpty() || id.size() > 64) return failure("invalid_effect", QStringLiteral("task name must contain 1 to 64 characters"));
    if (kind == QLatin1String("cancel")) {
        if (auto* task = m_tasks.value(owner).value(id)) finish(task, failure("cancelled", QStringLiteral("Operation cancelled")));
        return {};
    }
    if (m_tasks.value(owner).contains(id)) return failure("busy", QStringLiteral("A task with this name already exists"));
    if (m_tasks.value(owner).size() >= 16) return failure("rate_limited", QStringLiteral("At most 16 active tasks per script"));
    int total = 0;
    for (const auto& tasks : m_tasks) total += tasks.size();
    if (total >= 128) return failure("rate_limited", QStringLiteral("Global task limit reached"));
    const int timeout = bounded(e, "timeout_ms", 30000, 1, 600000);
    const int maxBytes = bounded(e, "max_bytes", 65536, 1, 1024 * 1024);
    if (timeout < 0 || maxBytes < 0) return failure("invalid_effect", QStringLiteral("Invalid timeout_ms or max_bytes"));
    auto* task = new Task(this);
    task->owner = owner; task->id = id; task->event = std::move(event);
    m_tasks[owner].insert(id, task);
    auto reject = [&](const QString& message) { finish(task, {}, false); return failure("invalid_effect", message); };
    if (kind == QLatin1String("timer")) {
        const int delay = bounded(e, "delay_ms", 1000, 1, 3600000);
        const int repeat = bounded(e, "repeat_ms", 0, 0, 3600000);
        if (delay < 0 || repeat < 0 || (repeat && repeat < 250)) return reject(QStringLiteral("Timers require delay_ms >= 1 and repeat_ms = 0 or >= 250"));
        auto* timer = new QTimer(task);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, task, [this, task, timer, repeat] {
            if (task->done) return;
            const auto event = task->event;
            const QVariantMap data{{QStringLiteral("name"), task->id}};
            if (!repeat) finish(task, {}, false);
            if (event) event(QStringLiteral("on_timer"), data);
            if (repeat && !task->done) timer->start(repeat);
        });
        timer->start(delay);
        return {};
    }
    if (kind == QLatin1String("watch_file")) {
        const QString path = QFileInfo(e.value(QStringLiteral("path")).toString()).absoluteFilePath();
        if (!QFileInfo(path).isFile()) return reject(QStringLiteral("watch_file requires an existing regular file"));
        auto* watcher = new QFileSystemWatcher(task);
        auto* debounce = new QTimer(task);
        debounce->setSingleShot(true); debounce->setInterval(250);
        watcher->addPaths({path, QFileInfo(path).absolutePath()});
        auto stamp = [path] {
            struct stat info{};
            if (::stat(QFile::encodeName(path).constData(), &info) != 0) return QByteArray();
            return QByteArray::number(info.st_ino) + ':' + QByteArray::number(info.st_size) + ':'
                + QByteArray::number(info.st_mtim.tv_sec) + ':' + QByteArray::number(info.st_mtim.tv_nsec);
        };
        auto previous = std::make_shared<QByteArray>(stamp());
        connect(watcher, &QFileSystemWatcher::fileChanged, debounce, qOverload<>(&QTimer::start));
        connect(watcher, &QFileSystemWatcher::directoryChanged, debounce, qOverload<>(&QTimer::start));
        connect(debounce, &QTimer::timeout, task, [task, watcher, path, stamp, previous] {
            if (task->done) return;
            const auto now = stamp();
            if (now == *previous) return;
            *previous = now;
            if (!now.isEmpty() && !watcher->files().contains(path)) watcher->addPath(path);
            task->event(QStringLiteral("on_file_changed"), {{QStringLiteral("name"), task->id},
                {QStringLiteral("path"), path}, {QStringLiteral("exists"), !now.isEmpty()}});
        });
        return {};
    }
    task->complete = std::move(complete);
    auto* timer = new QTimer(task);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, task, [this, task] { finish(task, failure("timeout", QStringLiteral("Operation timed out"))); });
    timer->start(timeout);
    if (kind == QLatin1String("read_file")) {
        const QString path = e.value(QStringLiteral("path")).toString();
        if (path.isEmpty() || path.size() > 4096 || path.contains(QChar(0))) return reject(QStringLiteral("Invalid file path"));
        if (!fileSlots.tryAcquire()) return reject(QStringLiteral("Too many file reads in flight"));
        auto* watcher = new QFutureWatcher<Result>(task);
        connect(watcher, &QFutureWatcherBase::finished, task, [this, task, watcher] { finish(task, watcher->result()); });
        watcher->setFuture(QtConcurrent::run([path, maxBytes, cancelled = task->cancelled] {
            struct Release { ~Release() { fileSlots.release(); } } release;
            if (*cancelled) return failure("cancelled", QStringLiteral("Operation cancelled"));
            const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0) return failure("io_error", QStringLiteral("Cannot open file"));
            struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
            struct stat info{};
            if (::fstat(fd, &info) || !S_ISREG(info.st_mode)) return failure("io_error", QStringLiteral("Only regular files can be read"));
            QByteArray bytes;
            while (bytes.size() <= maxBytes && !*cancelled) {
                char chunk[8192];
                const auto count = ::read(fd, chunk, std::size_t(std::min(qsizetype(sizeof chunk), maxBytes + 1 - bytes.size())));
                if (count < 0) return failure("io_error", QStringLiteral("Cannot read file"));
                if (count == 0) break;
                bytes.append(chunk, qsizetype(count));
            }
            const bool truncated = bytes.size() > maxBytes;
            if (truncated) {
                int end = maxBytes;
                while (end > 0 && (static_cast<unsigned char>(bytes[end]) & 0xc0) == 0x80) --end;
                bytes.truncate(end);
            }
            return Result{{}, {{QStringLiteral("text"), QString::fromUtf8(bytes)}, {QStringLiteral("truncated"), truncated}}, {}};
        }));
        return {{}, {}, {}, true};
    }
    if (kind != QLatin1String("exec")) return reject(QStringLiteral("Unknown task kind"));
    auto command = e.value(QStringLiteral("command")).toStringList();
    if (command.isEmpty() || command.size() > 64) return reject(QStringLiteral("exec needs an argv list of 1 to 64 strings"));
    for (const auto& arg : command) if (arg.size() > 4096 || arg.contains(QChar(0))) return reject(QStringLiteral("Invalid command argument"));
    auto* process = new QProcess(task);
    task->process = process;
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
    process->setWorkingDirectory(e.value(QStringLiteral("cwd")).toString());
    auto read = [this, task, maxBytes](bool stderrChannel) {
        if (task->done) return;
        auto* process = task->process;
        process->setReadChannel(stderrChannel ? QProcess::StandardError : QProcess::StandardOutput);
        const auto left = maxBytes - task->output.size() - task->errors.size();
        const auto bytes = process->read(left + 1);
        auto& target = stderrChannel ? task->errors : task->output;
        target += bytes.left(left);
        if (bytes.size() > left) {
            task->truncated = true;
            finish(task, {QStringLiteral("Process output limit reached"),
                {{QStringLiteral("stdout"), QString::fromUtf8(task->output)}, {QStringLiteral("stderr"), QString::fromUtf8(task->errors)},
                 {QStringLiteral("truncated"), true}}, QStringLiteral("output_limit")});
        }
    };
    connect(process, &QProcess::readyReadStandardOutput, task, [read] { read(false); });
    connect(process, &QProcess::readyReadStandardError, task, [read] { read(true); });
    connect(process, &QProcess::errorOccurred, task, [this, task](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(task, failure("spawn_failed", task->process->errorString()));
    });
    connect(process, &QProcess::finished, task, [this, task, read](int code, QProcess::ExitStatus status) {
        read(false); read(true);
        if (task->done) return;
        const bool ok = status == QProcess::NormalExit && code == 0;
        finish(task, {ok ? QString() : QStringLiteral("Process exited unsuccessfully"),
            {{QStringLiteral("stdout"), QString::fromUtf8(task->output)}, {QStringLiteral("stderr"), QString::fromUtf8(task->errors)},
             {QStringLiteral("exit_code"), code}, {QStringLiteral("truncated"), false}}, ok ? QString() : QStringLiteral("process_failed")});
    });
    process->setProgram(command.takeFirst()); process->setArguments(command); process->start();
    process->closeWriteChannel();
    return {{}, {}, {}, true};
}
}
