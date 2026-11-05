// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QPoint>
#include <cstdint>
#include <functional>
#include <memory>
class QWindow;
class QDropEvent;
namespace Afterglow {
/// Kitty OSC 72 desktop bridge. Each terminal pane owns its own protocol state.
class DragDrop : public QObject {
    Q_OBJECT
public:
    /// What the commands that the pane dropped held, in the order that the
    /// program wrote them.
    enum class Lost : std::uint8_t {
        Nothing, ///< no OSC 72 packet and no terminal reset
        Reset, ///< a terminal reset, and no OSC 72 packet after it
        Packets, ///< an OSC 72 packet after the last terminal reset
    };

    explicit DragDrop(QObject* parent = nullptr);
    ~DragDrop() override;
    /// The default of Terminal.dragAnswerWait.
    static constexpr int DefaultAnswerWaitMs = 100;
    static QByteArray queryReply(const QByteArray& command);
    void handle(const QByteArray& command);
    /// A terminal reset: ends the protocol and every message, so the next packet starts a new one.
    void reset();
    /// Ends the protocol, as when a reply cannot be written. The rest of a
    /// message that the program is sending is dropped up to its last chunk.
    void end();
    /// The pane dropped commands. When @p lost holds OSC 72 packets, this
    /// ends the protocol as end() does, sends ENOMEM first to a drop or a drag
    /// that is active (unless @p reset: a lost reset ended them), and drops the
    /// packets that follow up to the next one with m=0. When @p lost ends with
    /// a terminal reset, this is reset(). With nothing lost, it does nothing.
    void abort(Lost lost, bool reset = false);
    /// Reports the bytes that wait in the PTY input queue. Drop data pauses while it is large.
    void setInputBacklog(std::function<std::size_t()> backlog);
    /// Delivers the program's protocol commands that wait for the GUI thread
    /// (OSC 72, OSC 5113, terminal resets and overflow notices), and no other
    /// events, so a drag event answers with what the program has said. With a
    /// wait in milliseconds above 0 and nothing to deliver, it first waits up
    /// to that long for the next command. It runs no event loop.
    void setCommandDelivery(std::function<void(int waitMs)> deliver);
    /// Milliseconds that a drag event that shows the program new types waits
    /// for its answer (Terminal.dragAnswerWait). 0 does not wait.
    void setAnswerWait(int ms);
    bool acceptsDrops() const;
    bool offersDrags() const;
    /// The last drag event over the pane accepted the drag, so the desktop
    /// has the program's acceptance.
    bool acceptsDrag() const;
    void drop(QDropEvent* event, bool final, QPoint cell, QPoint pixel);
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
