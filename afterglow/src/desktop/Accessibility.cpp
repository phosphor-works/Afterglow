// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Accessibility.h"
#include "../TerminalWindow.h"
#include <QAccessibleObject>
#include <QGuiApplication>
#include <QQuickItem>
#include <QPointer>
#include <optional>
#include <private/qaccessiblequickitem_p.h>

namespace Afterglow {
namespace {
TerminalWindow* owner(Pane* pane)
{
    for (auto* w : TerminalWindow::windows()) if (w->panes().contains(pane)) return w;
    return nullptr;
}
class AccessiblePane final : public QAccessibleObject, public QAccessibleTextInterface {
public:
    explicit AccessiblePane(Pane* p) : QAccessibleObject(p) {
        m_output = QObject::connect(p, &Pane::contentChanged, p, [this] { m_cached.reset(); });
    }
    ~AccessiblePane() override { QObject::disconnect(m_output); }
    Pane* pane() const { return qobject_cast<Pane*>(object()); }
    // A query loop asks for each character in turn. One capture serves it until the
    // content changes or the next frame refreshes it.
    TerminalText snapshot() const {
        if (!m_cached) m_cached = pane()->accessibleText();
        return *m_cached;
    }
    void invalidate() { m_cached.reset(); }
    QWindow* window() const override { return owner(pane()); }
    QAccessibleInterface* parent() const override { return QAccessible::queryAccessibleInterface(window()); }
    QAccessibleInterface* child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface*) const override { return -1; }
    QRect rect() const override { auto* w = owner(pane()); return w ? w->accessiblePaneRect(pane()) : QRect(); }
    QString text(QAccessible::Text kind) const override {
        if (kind == QAccessible::Name) return pane()->title().isEmpty() ? QStringLiteral("Terminal") : pane()->title();
        if (kind == QAccessible::Value) return snapshot().text;
        if (kind == QAccessible::Description) return QStringLiteral("Terminal viewport. Scroll to read history.");
        return {};
    }
    QAccessible::Role role() const override { return QAccessible::Terminal; }
    QAccessible::State state() const override {
        QAccessible::State s;
        auto* w = owner(pane());
        s.selectableText = true; s.multiLine = true;
        s.readOnly = true; s.invisible = !w || !w->isVisible() || w->accessiblePaneRect(pane()).isEmpty();
        s.focusable = !s.invisible;
        s.focused = w && w->isActive() && w->focusedPane() == pane();
        return s;
    }
    void* interface_cast(QAccessible::InterfaceType t) override {
        return t == QAccessible::TextInterface ? static_cast<QAccessibleTextInterface*>(this) : nullptr;
    }
    int characterCount() const override { return int(snapshot().text.size()); }
    QString text(int start, int end) const override {
        const auto s = snapshot();
        if (end == -1) end = int(s.text.size());
        if (start < 0 || end < start || end > s.text.size()) return {};
        return s.text.mid(start, end - start);
    }
    int cursorPosition() const override { return snapshot().cursor; }
    // The shell owns its editing cursor; accessibility must never rewrite output.
    void setCursorPosition(int) override {}
    int selectionCount() const override { return snapshot().selectionStart >= 0 ? 1 : 0; }
    void selection(int index, int* start, int* end) const override {
        const auto s = snapshot();
        *start = index == 0 ? s.selectionStart : -1; *end = index == 0 ? s.selectionEnd : -1;
    }
    void addSelection(int start, int end) override { setSelection(0, start, end); }
    void setSelection(int index, int start, int end) override {
        const auto s = snapshot();
        if (index != 0 || start < 0 || end < start || end > s.cells.size()) return;
        if (start == end) { removeSelection(0); return; }
        pane()->startSelection(s.cells[start], AfterglowVt::SelectionMode::Normal, true);
        pane()->extendSelection(s.cells[end - 1]);
        m_cached.reset();
        Q_EMIT pane()->contentChanged();
    }
    void removeSelection(int index) override {
        if (index != 0 || snapshot().selectionStart < 0) return;
        pane()->clearSelection();
        m_cached.reset();
        Q_EMIT pane()->contentChanged();
    }
    QRect characterRect(int offset) const override {
        const auto s = snapshot();
        if (offset < 0 || offset >= s.cells.size()) return {};
        auto* w = owner(pane());
        return w ? w->accessibleCellRect(pane(), s.cells[offset]) : QRect();
    }
    int offsetAtPoint(const QPoint& point) const override {
        if (!rect().contains(point)) return -1;
        const auto s = snapshot();
        auto* w = owner(pane());
        for (int i = 0; w && i < s.cells.size(); ++i)
            if (w->accessibleCellRect(pane(), s.cells[i]).contains(point)) return i;
        return -1;
    }
    void scrollToSubstring(int start, int end) override {
        if (start < 0 || end < start || end > characterCount()) return;
        if (auto* w = owner(pane())) w->revealPane(pane());
    }
    QString attributes(int offset, int* start, int* end) const override {
        const int count = characterCount();
        if (offset < 0 || offset >= count) { *start = *end = -1; return {}; }
        *start = 0; *end = count; return QStringLiteral("read-only:true;");
    }
    void notify() {
        // Nothing an assistive technology reads changed: no text to rebuild.
        const auto serial = pane()->accessibleSerial();
        if (m_notified == serial) return;
        m_notified = serial;
        m_cached = pane()->accessibleText();
        const auto next = *m_cached;
        for (const auto& edit : textEdits(previous.text, next.text)) {
            if (edit.inserted.isEmpty()) {
                QAccessibleTextRemoveEvent event(pane(), edit.position, edit.removed);
                QAccessible::updateAccessibility(&event);
            } else if (edit.removed.isEmpty()) {
                QAccessibleTextInsertEvent event(pane(), edit.position, edit.inserted);
                QAccessible::updateAccessibility(&event);
            } else {
                QAccessibleTextUpdateEvent event(pane(), edit.position, edit.removed, edit.inserted);
                QAccessible::updateAccessibility(&event);
            }
        }
        if (next.cursor != previous.cursor) {
            QAccessibleTextCursorEvent event(pane(), next.cursor); QAccessible::updateAccessibility(&event);
        }
        if (next.selectionStart != previous.selectionStart || next.selectionEnd != previous.selectionEnd) {
            QAccessibleTextSelectionEvent event(pane(), next.selectionStart, next.selectionEnd); QAccessible::updateAccessibility(&event);
        }
        previous = next;
    }
    TerminalText previous;
private:
    mutable std::optional<TerminalText> m_cached;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> m_notified;
    QMetaObject::Connection m_output;
};
class AccessibleWindow final : public QAccessibleObject {
public:
    explicit AccessibleWindow(TerminalWindow* w) : QAccessibleObject(w) {}
    TerminalWindow* terminal() const { return qobject_cast<TerminalWindow*>(object()); }
    QWindow* window() const override { return terminal(); }
    QList<Pane*> visiblePanes() const {
        QList<Pane*> result;
        for (auto* p : terminal()->panes()) if (!terminal()->accessiblePaneRect(p).isEmpty()) result.append(p);
        return result;
    }
    QAccessibleInterface* parent() const override { return QAccessible::queryAccessibleInterface(qApp); }
    int childCount() const override { return int(visiblePanes().size()) + (terminal()->accessibleChrome() ? 1 : 0); }
    QAccessibleInterface* child(int i) const override { const auto p = visiblePanes(); return i >= 0 && i < p.size() ? QAccessible::queryAccessibleInterface(p[i]) : i == p.size() ? terminal()->accessibleChrome() : nullptr; }
    int indexOfChild(const QAccessibleInterface* c) const override { return c && c == terminal()->accessibleChrome() ? int(visiblePanes().size()) : int(visiblePanes().indexOf(qobject_cast<Pane*>(c ? c->object() : nullptr))); }
    QAccessibleInterface* childAt(int x, int y) const override {
        if (auto* chrome = terminal()->accessibleChrome()) if (auto* control = chrome->childAt(x, y)) return control;
        for (int i = 0; i < childCount(); ++i) if (child(i)->rect().contains(x, y)) return child(i);
        return nullptr;
    }
    QAccessibleInterface* focusChild() const override {
        if (auto* focus = terminal()->accessibleChromeFocus()) return focus;
        return QAccessible::queryAccessibleInterface(terminal()->focusedPane());
    }
    QRect rect() const override { return QRect(terminal()->mapToGlobal(QPoint()), terminal()->size()); }
    QString text(QAccessible::Text t) const override { return t == QAccessible::Name ? terminal()->title() : QString(); }
    QAccessible::Role role() const override { return QAccessible::Window; }
    QAccessible::State state() const override { QAccessible::State s; s.active = terminal()->isActive(); s.invisible = !terminal()->isVisible(); return s; }
};
}
void installChromeAccessibility(QQuickItem* item, QWindow* host)
{
    class ChromeRoot final : public QAccessibleQuickItem {
    public:
        ChromeRoot(QQuickItem* item, QWindow* host) : QAccessibleQuickItem(item), m_host(host) {}
        QWindow* window() const override { return m_host; }
        QAccessibleInterface* parent() const override { return QAccessible::queryAccessibleInterface(m_host); }
    private:
        QPointer<QWindow> m_host;
    };
    if (auto* old = QAccessible::queryAccessibleInterface(item))
        QAccessible::deleteAccessibleInterface(QAccessible::uniqueId(old));
    QAccessible::registerAccessibleInterface(new ChromeRoot(item, host));
}

void installTerminalAccessibility()
{
    static const bool installed = [] {
        QAccessible::installFactory([](const QString&, QObject* o) -> QAccessibleInterface* {
            if (auto* w = qobject_cast<TerminalWindow*>(o)) return new AccessibleWindow(w);
            if (auto* p = qobject_cast<Pane*>(o)) return new AccessiblePane(p);
            return nullptr;
        });
        return true;
    }();
    Q_UNUSED(installed);
}
void updateTerminalAccessibility(TerminalWindow* w)
{
    if (!QAccessible::isActive()) return;
    for (auto* p : w->panes()) {
        auto* a = dynamic_cast<AccessiblePane*>(QAccessible::queryAccessibleInterface(p));
        if (!a) continue;
        // Background tabs and panes hidden by zoom are not in the tree and send no events.
        if (w->tabIdOf(p) == w->activeTabId() && !w->accessiblePaneRect(p).isEmpty()) a->notify();
        else a->invalidate();
    }
}
}
