// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <chrono>
#include <deque>
#include <functional>
#include <vector>

namespace Afterglow {

/// File paths in terminal text: the candidates that the mouse and the
/// default hint pattern find, how they resolve, and how they open.
namespace PathLinks {

/// The regular expression of a path, with an optional :LINE or :LINE:COLUMN.
/// Mouse detection and the default hint pattern share it. PCRE2 syntax, for
/// QRegularExpression with Unicode properties and for AfterglowVt::searchText.
const QString& pattern();

/// A path and the position in it that a trailing :LINE or :LINE:COLUMN names.
struct Location
{
    QString path;
    int line = 0; ///< 1-based; 0 when the text names none
    int column = 0; ///< 1-based; 0 when the text names none
    bool operator==(const Location&) const = default;
};
/// Splits a trailing :LINE or :LINE:COLUMN off @p text.
Location parseLocation(const QString& text);

/// One path candidate in a line of text, in UTF-16 units.
struct Candidate
{
    qsizetype start = 0;
    qsizetype length = 0;
    Location location;
};
/// The path candidates in @p text, left to right.
std::vector<Candidate> find(const QString& text);

/// @p path as a clean absolute path: "~/" is @p home, and a relative path is
/// taken from @p directory. Empty when @p path is relative and @p directory
/// is not absolute.
QString resolve(const QString& path, const QString& directory, const QString& home);

/// Replaces "{name}" in each argument with @p values[name], in one pass:
/// text that a value brings in ("{line}" in a file name) stays as it is.
/// Placeholders that @p values does not name stay too.
QStringList substitute(const QStringList& arguments, const QHash<QString, QString>& values);

/// How to open a file or a directory at @p file (absolute).
struct OpenAction
{
    QStringList command; ///< the program and its arguments; empty to open url
    QUrl url; ///< file:// and the path, when command is empty
};
/// Links.openFilesWith with {file}, {line} and {column} filled in. A missing
/// line or column is 1. A directory, or an empty @p openFilesWith, opens its
/// file:// URL instead, with the desktop's application or the file manager.
OpenAction openAction(const QStringList& openFilesWith, const Location& file, bool directory);

/// What a path names on this machine.
enum class Kind { Unknown, Missing, File, Directory };

/// Checks whether paths exist on worker threads, and keeps the answers for a
/// short time. A stat on a hung network mount blocks its worker, never the
/// GUI thread. GUI thread only.
class Checker : public QObject
{
    Q_OBJECT
public:
    explicit Checker(QObject* parent = nullptr, std::chrono::milliseconds lifetime = std::chrono::seconds(5),
                     int capacity = 1024);
    /// The cached kind of the absolute @p path. Unknown starts a check, if
    /// none runs for it, and checked() tells when it is known.
    Kind lookup(const QString& path);
    /// Calls @p done with the kind of @p path, now from the cache or after a
    /// check, unless @p context is gone by then. Unknown when too many checks
    /// wait already.
    void check(const QString& path, QObject* context, std::function<void(Kind)> done);
    /// Checks that wait for a worker or run.
    int pending() const { return int(m_waiting.size()); }

Q_SIGNALS:
    void checked(const QString& path, Afterglow::PathLinks::Kind kind);

private:
    bool start(const QString& path);
    void finish(const QString& path, Kind kind);
    struct Entry
    {
        Kind kind = Kind::Unknown;
        std::chrono::steady_clock::time_point time;
    };
    struct Waiter
    {
        QPointer<QObject> context;
        std::function<void(Kind)> done;
    };
    std::chrono::milliseconds m_lifetime;
    int m_capacity;
    QHash<QString, Entry> m_cache;
    std::deque<QString> m_order; ///< m_cache's keys, oldest first
    QHash<QString, std::vector<Waiter>> m_waiting; ///< checks that run, with the calls that wait for them
};

} // namespace PathLinks
} // namespace Afterglow
