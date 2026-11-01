// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QPoint>
#include <functional>
#include <memory>
class QWindow;
class QDropEvent;
namespace Afterglow {
/// Kitty OSC 72 desktop bridge. Each terminal pane owns its own protocol state.
class DragDrop : public QObject {
    Q_OBJECT
public:
    explicit DragDrop(QObject* parent = nullptr);
    ~DragDrop() override;
    static QByteArray queryReply(const QByteArray& command);
    void handle(const QByteArray& command);
    void reset();
    void abort();
    /// Reports the bytes that wait in the PTY input queue. Drop data pauses while it is large.
    void setInputBacklog(std::function<std::size_t()> backlog);
    bool acceptsDrops() const;
    bool offersDrags() const;
    void drop(QDropEvent* event, bool final, QPoint cell, QPoint pixel, QWindow* window);
    void leave();
    void press(QPoint position);
    void release();
    bool move(QWindow* window, QPoint position, QPoint cell, QPoint pixel);
Q_SIGNALS:
    void reply(const QByteArray& bytes);

private:
    struct State;
    std::shared_ptr<State> d;
};
} // namespace Afterglow
