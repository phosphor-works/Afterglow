// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScriptTasks.h"
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <PhosphorFsLoader/WatchedFileSet.h>

#include <QFutureWatcher>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
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
constexpr int FileWorkers = 8;
QSemaphore fileSlots(FileWorkers);
// Reads that block in the kernel (a stalled network mount) keep their thread,
// so they get their own pool and never starve the global one.
QThreadPool* filePool()
{
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(FileWorkers);
        return p;
    }();
    return pool;
}
/// Bytes cut at a byte limit, without a partial UTF-8 sequence at the end.
QString utf8Prefix(const QByteArray& bytes, qsizetype limit)
{
    if (bytes.size() <= limit) return QString::fromUtf8(bytes);
    qsizetype end = limit;
    while (end > 0 && (static_cast<unsigned char>(bytes[end]) & 0xc0) == 0x80) --end;
    return QString::fromUtf8(bytes.left(end));
}
}
struct ScriptTasks::Task : QObject {
    QString owner, id;
    Completion complete;
    Event event;
    QProcess* process = nullptr;
    qint64 group = 0; ///< the process group of a started job, which outlives its leader
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
    // Children the job left behind stay in its group after a normal exit.
    if (task->group > 0) ::kill(-pid_t(task->group), SIGKILL);
    if (auto* process = task->process; process && process->state() != QProcess::NotRunning) {
        // Deleting a running QProcess waits for it on this thread. Let it
        // reap the killed process in the background instead.
        process->disconnect(task);
        process->setParent(nullptr);
        connect(process, &QProcess::finished, process, &QObject::deleteLater);
        process->kill();
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
        const QString path = e.value(QStringLiteral("path")).toString();
        if (!QDir::isAbsolutePath(path)) return reject(QStringLiteral("watch_file requires an absolute path"));
        if (!QFileInfo(path).isFile()) return reject(QStringLiteral("watch_file requires an existing regular file"));
        auto* watcher = new PhosphorFsLoader::WatchedFileSet(task);
        watcher->setDebounceInterval(250);
        watcher->setPaths({path});
        connect(watcher, &PhosphorFsLoader::WatchedFileSet::filesChanged, task, [task, path] {
            if (task->done) return;
            task->event(QStringLiteral("on_file_changed"), {{QStringLiteral("name"), task->id},
                {QStringLiteral("path"), path}, {QStringLiteral("exists"), QFileInfo::exists(path)}});
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
        if (path.size() > 4096 || path.contains(QChar(0)) || !QDir::isAbsolutePath(path))
            return reject(QStringLiteral("read_file needs an absolute path of at most 4096 characters"));
        if (!fileSlots.tryAcquire()) {
            finish(task, {}, false);
            return failure("busy", QStringLiteral("Too many file reads in flight"));
        }
        auto* watcher = new QFutureWatcher<Result>(task);
        connect(watcher, &QFutureWatcherBase::finished, task, [this, task, watcher] { finish(task, watcher->result()); });
        watcher->setFuture(QtConcurrent::run(filePool(), [path, maxBytes, cancelled = task->cancelled] {
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
                if (count < 0 && errno == EINTR) continue;
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
    const QString cwd = e.value(QStringLiteral("cwd")).toString();
    if (!cwd.isEmpty() && (cwd.contains(QChar(0)) || !QDir::isAbsolutePath(cwd)))
        return reject(QStringLiteral("exec cwd must be an absolute path"));
    auto* process = new QProcess(task);
    task->process = process;
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
    process->setWorkingDirectory(cwd);
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto overrides = e.value(QStringLiteral("env")).toMap();
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        if (it.value().typeId() == QMetaType::Bool) environment.remove(it.key());
        else environment.insert(it.key(), it.value().toString());
    }
    process->setProcessEnvironment(environment);
    if (!command.front().contains(QLatin1Char('/'))) {
        const QDir working(cwd.isEmpty() ? QDir::currentPath() : cwd);
        QStringList paths;
        const auto path = environment.contains(QStringLiteral("PATH")) ? environment.value(QStringLiteral("PATH"))
            : QStringLiteral("/bin:/usr/bin");
        for (const auto& entry : path.split(QLatin1Char(':'), Qt::KeepEmptyParts))
            paths.append(working.absoluteFilePath(entry.isEmpty() ? QStringLiteral(".") : entry));
        const auto executable = QStandardPaths::findExecutable(command.front(), paths);
        if (executable.isEmpty()) {
            QTimer::singleShot(0, task, [this, task] { finish(task, failure("spawn_failed", QStringLiteral("Executable not found in child PATH"))); });
            return {{}, {}, {}, true};
        }
        command.front() = executable;
    }
    auto read = [this, task, maxBytes](bool stderrChannel) {
        if (task->done) return;
        auto* process = task->process;
        process->setReadChannel(stderrChannel ? QProcess::StandardError : QProcess::StandardOutput);
        const auto left = maxBytes - task->output.size() - task->errors.size();
        const auto bytes = process->read(left + 1);
        auto& target = stderrChannel ? task->errors : task->output;
        if (bytes.size() > left) {
            task->truncated = true;
            const auto cut = target.size() + left;
            target += bytes;
            const auto& other = stderrChannel ? task->output : task->errors;
            const QString kept = utf8Prefix(target, cut);
            const QString out = stderrChannel ? QString::fromUtf8(other) : kept;
            const QString err = stderrChannel ? kept : QString::fromUtf8(other);
            finish(task, {QStringLiteral("Process output limit reached"),
                {{QStringLiteral("stdout"), out}, {QStringLiteral("stderr"), err}, {QStringLiteral("truncated"), true}},
                QStringLiteral("output_limit")});
            return;
        }
        target += bytes;
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
    connect(process, &QProcess::started, task, [task] { task->group = task->process->processId(); });
    process->setProgram(command.takeFirst()); process->setArguments(command); process->start();
    process->closeWriteChannel();
    return {{}, {}, {}, true};
}
}
