// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/PathLinks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThreadPool>

#include <algorithm>

namespace Afterglow::PathLinks {

namespace {
// Paths are names on this machine: longer ones are no path that a stat
// could find.
constexpr qsizetype MaxPathLength = 4096;
// Checks that run or wait at one time. Stats that hang on a network mount
// keep their slot, and then new paths are not checked until one returns.
constexpr qsizetype MaxRunning = 64;

// Stats that block in the kernel (a stalled network mount) keep their
// thread: they get their own pool, and never starve the global one. The pool
// is never deleted, since its destructor would wait for such a stat.
QThreadPool* statPool()
{
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(4);
        return p;
    }();
    return pool;
}
} // namespace

const QString& pattern()
{
    // Four forms, each with an optional :LINE or :LINE:COLUMN:
    //   - absolute, or from ~/, ./ or ../;
    //   - relative with a slash;
    //   - a name with an extension that has a letter (not "1.5");
    //   - a name with a letter, followed by a line ("Makefile:12", not "12:30").
    // No form ends on a full stop, and none takes other sentence punctuation
    // (, ; : ! ? quotes, brackets): the same characters that URLs drop from
    // their end. Upper-case letters would make hint searches match case.
    static const QString p = QStringLiteral(
        R"((?:(?:~|\.{1,2})?/(?:[\w.@+-]+/)*[\w.@+-]*[\w@+-]/?)"
        R"(|(?:[\w.@+-]+/)+(?:[\w.@+-]*[\w@+-]/?)?)"
        R"(|[\w.@+-]*\.(?=[\w+-]*[^\W\d])[\w+-]+)"
        R"(|(?=[\w.@+-]*[^\W\d_])[\w.@+-]*[\w@+-](?=:\d)))"
        R"((?::\d+){0,2})");
    return p;
}

Location parseLocation(const QString& text)
{
    static const QRegularExpression location(QStringLiteral(R"(^(.+?):(\d+)(?::(\d+))?$)"));
    const QRegularExpressionMatch match = location.match(text);
    if (!match.hasMatch()) return {text, 0, 0};
    bool lineOk = false, columnOk = false;
    const int line = match.captured(2).toInt(&lineOk);
    const int column = match.captured(3).toInt(&columnOk);
    return {match.captured(1), lineOk ? line : 0, lineOk && columnOk ? column : 0};
}

std::vector<Candidate> find(const QString& text)
{
    static const QRegularExpression expression(pattern(), QRegularExpression::UseUnicodePropertiesOption);
    std::vector<Candidate> found;
    for (auto it = expression.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (match.capturedLength() > MaxPathLength) continue;
        found.push_back({match.capturedStart(), match.capturedLength(), parseLocation(match.captured())});
    }
    return found;
}

QString resolve(const QString& path, const QString& directory, const QString& home)
{
    if (path.isEmpty() || path.size() > MaxPathLength || path.contains(QChar(0))) return {};
    QString absolute;
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/"))) {
        if (!QDir::isAbsolutePath(home)) return {};
        absolute = home + path.mid(1);
    } else if (QDir::isAbsolutePath(path)) {
        absolute = path;
    } else if (QDir::isAbsolutePath(directory)) {
        absolute = directory + QLatin1Char('/') + path;
    } else {
        return {};
    }
    return QDir::cleanPath(absolute);
}

QStringList substitute(const QStringList& arguments, const QHash<QString, QString>& values)
{
    static const QRegularExpression placeholder(QStringLiteral(R"(\{(\w+)\})"));
    QStringList out;
    out.reserve(arguments.size());
    for (const QString& argument : arguments) {
        QString text;
        qsizetype last = 0;
        for (auto it = placeholder.globalMatch(argument); it.hasNext();) {
            const QRegularExpressionMatch match = it.next();
            const auto value = values.constFind(match.captured(1));
            if (value == values.cend()) continue;
            text += argument.mid(last, match.capturedStart() - last) + *value;
            last = match.capturedEnd();
        }
        out << text + argument.mid(last);
    }
    return out;
}

OpenAction openAction(const QStringList& openFilesWith, const Location& file, bool directory)
{
    if (directory || openFilesWith.isEmpty()) return {{}, QUrl::fromLocalFile(file.path)};
    return {substitute(openFilesWith, {{QStringLiteral("file"), file.path},
                                       {QStringLiteral("line"), QString::number(file.line > 0 ? file.line : 1)},
                                       {QStringLiteral("column"), QString::number(file.column > 0 ? file.column : 1)}}),
            {}};
}

Checker::Checker(QObject* parent, std::chrono::milliseconds lifetime, int capacity)
    : QObject(parent)
    , m_lifetime(lifetime)
    , m_capacity(std::max(capacity, 1))
{
}

Kind Checker::lookup(const QString& path)
{
    if (const auto it = m_cache.constFind(path);
        it != m_cache.cend() && std::chrono::steady_clock::now() - it->time < m_lifetime) {
        return it->kind;
    }
    start(path);
    return Kind::Unknown;
}

void Checker::check(const QString& path, QObject* context, std::function<void(Kind)> done)
{
    if (const Kind kind = lookup(path); kind != Kind::Unknown) {
        done(kind);
        return;
    }
    const auto waiting = m_waiting.find(path);
    if (waiting == m_waiting.end()) {
        done(Kind::Unknown); // too many checks wait already
        return;
    }
    waiting->push_back({context, std::move(done)});
}

bool Checker::start(const QString& path)
{
    if (m_waiting.contains(path)) return true;
    if (path.isEmpty() || m_waiting.size() >= MaxRunning) return false;
    m_waiting.insert(path, {});
    statPool()->start([self = QPointer<Checker>(this), path] {
        const QFileInfo info(path);
        const Kind kind = info.isDir() ? Kind::Directory : info.exists() ? Kind::File : Kind::Missing;
        // The application outlives every window and its checker. The guard
        // is read on the GUI thread, where the checker is deleted.
        if (auto* app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(app, [self, path, kind] {
                if (self) self->finish(path, kind);
            }, Qt::QueuedConnection);
        }
    });
    return true;
}

void Checker::finish(const QString& path, Kind kind)
{
    if (!m_cache.contains(path)) m_order.push_back(path);
    m_cache.insert(path, {kind, std::chrono::steady_clock::now()});
    while (qsizetype(m_order.size()) > m_capacity) {
        m_cache.remove(m_order.front());
        m_order.pop_front();
    }
    const std::vector<Waiter> waiters = m_waiting.take(path);
    Q_EMIT checked(path, kind);
    for (const Waiter& waiter : waiters) {
        if (waiter.context) waiter.done(kind);
    }
}

} // namespace Afterglow::PathLinks
