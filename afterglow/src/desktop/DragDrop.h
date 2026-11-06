// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QDeadlineTimer>
#include <QObject>
#include <QPoint>
#include <QTimer>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
class QWindow;
class QDropEvent;
class QTemporaryDir;
namespace Afterglow {
/// The directories that hold the files of remote drags: what a drag source
/// on another computer sent, for the drop target to read here. The process
/// owns them, not the pane. A drop target such as a file manager copies the
/// files after the drag has ended, and can still copy after the pane closed
/// or the next drag started.
class DragTrees : public QObject {
public:
    /// The default of Terminal.dragTreeLifetime.
    static constexpr std::chrono::minutes DefaultLifetime { 10 };
    static DragTrees& instance();
    ~DragTrees() override;
    /// How long the tree of a drag that ended in a drop remains. A tree keeps
    /// the lifetime that was set when its drag ended.
    void setLifetime(std::chrono::milliseconds lifetime);
    /// A new directory for a drag that starts, kept until finish() or
    /// remove(). Null when it cannot be made.
    std::shared_ptr<QTemporaryDir> create();
    /// The drag ended in a drop: the tree remains for the lifetime.
    void finish(const std::shared_ptr<QTemporaryDir>& tree);
    /// The drag was canceled or failed: nothing reads the tree, so it goes now.
    void remove(const std::shared_ptr<QTemporaryDir>& tree);
    /// Whether the file at @p path is inside one of the trees. Links in the
    /// directories above it are followed, a link that it is itself is not.
    bool holds(const QByteArray& path) const;
    std::size_t size() const { return m_trees.size(); }

private:
    explicit DragTrees(QObject* parent);
    void expire();
    struct Tree {
        std::shared_ptr<QTemporaryDir> directory;
        QByteArray root; ///< the directory's path with no links
        QDeadlineTimer expiry; ///< forever while the drag runs
    };
    std::vector<Tree> m_trees;
    QTimer m_timer;
    std::chrono::milliseconds m_lifetime = DefaultLifetime;
};
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
