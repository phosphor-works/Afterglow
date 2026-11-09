// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QPointer>
#include <QWindow>

namespace Afterglow {

/// An event loop that runs inside a handler of a window: a QDrag::exec(), or
/// a drop that waits for its data from a program. Events go on meanwhile, so
/// the window can be asked to close, but it must not be deleted before the
/// handler returns: deleteWhenIdle() waits for the last loop. GUI thread only.
class NestedLoop {
public:
    explicit NestedLoop(QWindow* window)
        : m_window(window)
        , m_key(window)
    {
        if (m_key) ++entry(window).count;
    }
    ~NestedLoop()
    {
        if (!m_key) return;
        const auto it = loops().find(m_key);
        if (it == loops().end() || --it->count > 0) return;
        const bool deletePending = it->deletePending;
        loops().erase(it);
        // Here the handler has returned from its loop: the window can go.
        if (deletePending && m_window) m_window->deleteLater();
    }
    NestedLoop(const NestedLoop&) = delete;
    NestedLoop& operator=(const NestedLoop&) = delete;

    /// Deletes @p window (deleteLater) now, or after its last loop ends.
    static void deleteWhenIdle(QWindow* window)
    {
        if (const auto it = loops().find(window); it != loops().end() && it->window) {
            it->deletePending = true;
            return;
        }
        window->deleteLater();
    }

private:
    struct Loops {
        QPointer<QWindow> window;
        int count = 0;
        bool deletePending = false;
    };
    /// The entry of @p window. An entry left by a window that was deleted
    /// while a loop ran is not this window's, though the address is the same.
    static Loops& entry(QWindow* window)
    {
        Loops& loops = NestedLoop::loops()[window];
        if (!loops.window) loops = {window, 0, false};
        return loops;
    }
    static QHash<const QWindow*, Loops>& loops()
    {
        static QHash<const QWindow*, Loops> all;
        return all;
    }
    QPointer<QWindow> m_window;
    const QWindow* m_key;
};

} // namespace Afterglow
