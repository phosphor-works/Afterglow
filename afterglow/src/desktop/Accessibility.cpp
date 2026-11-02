// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Accessibility.h"
#include "../DockArea.h"
#include "../TerminalWindow.h"
#include "../config/Keybinds.h"
#include <QHash>
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
// The palette's actions that read or change one pane, or start from it (a
// split, tab or window opens in its directory). The others act the same from
// every pane: tabs, fonts, fullscreen, equalize and rotate, the overview,
// shaders, projects, the configuration and the palette itself. The tab bar
// and the palette reach those, and on each pane they would bury its own.
// End Script Key Mode is the chord that frees the keys, not a pane action.
bool paneAction(Action action)
{
    switch (action) {
    case Action::Copy: case Action::Paste: case Action::SelectAll: case Action::Search:
    case Action::ClearScreen: case Action::ResetTerminal: case Action::RenamePane: case Action::NotifyOnFinish:
    case Action::CopyMode: case Action::ShowHints: case Action::ShowLastOutput: case Action::UrlsOpen:
    case Action::UrlsCopy: case Action::UnicodeInput:
    case Action::ScrollPageUp: case Action::ScrollPageDown: case Action::ScrollTop: case Action::ScrollBottom:
    case Action::JumpPromptUp: case Action::JumpPromptDown:
    case Action::SplitRight: case Action::SplitDown: case Action::SplitLeft: case Action::SplitUp:
    case Action::ClosePane: case Action::TogglePaneZoom:
    case Action::ResizeLeft: case Action::ResizeRight: case Action::ResizeUp: case Action::ResizeDown:
    case Action::SwapLeft: case Action::SwapRight: case Action::SwapUp: case Action::SwapDown:
    case Action::FocusLeft: case Action::FocusRight: case Action::FocusUp: case Action::FocusDown:
    case Action::MovePaneToTab: case Action::MovePaneToWindow: case Action::NewTab: case Action::NewWindow:
        return true;
    default:
        return false;
    }
}
const QList<Action>& paneActions()
{
    // In the palette's order.
    static const QList<Action> actions = [] {
        QList<Action> result;
        for (Action a : Keybinds::allActions()) if (paneAction(a)) result.append(a);
        return result;
    }();
    return actions;
}
Action paneActionNamed(const QString& name)
{
    for (Action a : paneActions()) if (Keybinds::token(a) == name) return a;
    return Action::None;
}
class AccessiblePane final : public QAccessibleObject, public QAccessibleTextInterface, public QAccessibleActionInterface {
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
        if (t == QAccessible::TextInterface) return static_cast<QAccessibleTextInterface*>(this);
        if (t == QAccessible::ActionInterface) return static_cast<QAccessibleActionInterface*>(this);
        return nullptr;
    }
    // The menu comes first: a client that runs the first action by default
    // opens the list of the others, not a copy or a split.
    QStringList actionNames() const override {
        QStringList names{showMenuAction()};
        for (Action a : paneActions()) names.append(Keybinds::token(a));
        return names;
    }
    QString localizedActionName(const QString& name) const override {
        const Action a = paneActionNamed(name);
        return a == Action::None ? QAccessibleActionInterface::localizedActionName(name) : Keybinds::title(a);
    }
    QString localizedActionDescription(const QString& name) const override {
        const Action a = paneActionNamed(name);
        return a == Action::None ? QAccessibleActionInterface::localizedActionDescription(name) : Keybinds::title(a);
    }
    QStringList keyBindingsForAction(const QString& name) const override {
        const Action a = paneActionNamed(name);
        auto* w = owner(pane());
        return a == Action::None || !w ? QStringList() : w->actionShortcuts(a);
    }
    // As the action's keys in this pane: the same confirmations and notices.
    void doAction(const QString& name) override {
        auto* w = owner(pane());
        if (!w) return;
        if (name == showMenuAction()) w->openPaneMenu(pane());
        else if (const Action a = paneActionNamed(name); a != Action::None) w->runAction(pane(), a);
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
class AccessibleDock;
// An item of a dock: a button that sends its script the click a left press
// sends. Its dock makes it and deletes it, before the dock goes or when the
// order of the dock's items changes. Until then it reads the item at its
// index, and goes inert once a new order is drawn.
class AccessibleDockItem final : public QAccessibleInterface, public QAccessibleActionInterface {
public:
    AccessibleDockItem(AccessibleDock* dock, std::size_t index, quint64 generation)
        : m_dock(dock), m_index(index), m_generation(generation) {}
    bool isValid() const override { return item(); }
    QObject* object() const override { return nullptr; }
    QWindow* window() const override;
    QAccessibleInterface* childAt(int, int) const override { return nullptr; }
    QAccessibleInterface* parent() const override;
    QAccessibleInterface* child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface*) const override { return -1; }
    QString text(QAccessible::Text kind) const override {
        const auto* i = item();
        if (kind != QAccessible::Name || !i) return {};
        // Blank cells name nothing: the id the script gave says what it is.
        const QString shown = i->text.trimmed();
        return shown.isEmpty() ? i->id : shown;
    }
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override;
    QAccessible::Role role() const override { return QAccessible::Button; }
    QAccessible::State state() const override {
        QAccessible::State s;
        s.invisible = rect().isEmpty();
        s.focusable = !s.invisible;
        return s;
    }
    void* interface_cast(QAccessible::InterfaceType t) override {
        return t == QAccessible::ActionInterface ? static_cast<QAccessibleActionInterface*>(this) : nullptr;
    }
    QStringList actionNames() const override { return {pressAction()}; }
    QStringList keyBindingsForAction(const QString&) const override { return {}; }
    void doAction(const QString& name) override;
    const DockArea::Item* item() const;
private:
    AccessibleDock* m_dock;
    std::size_t m_index;
    quint64 m_generation;
};
// A script's dock: its text, and its items as buttons. A dock is not a
// QObject, so the window's accessible object makes these and the dock's
// window forgets them before the dock goes.
class AccessibleDock final : public QAccessibleInterface {
public:
    AccessibleDock(TerminalWindow* w, const DockArea* d) : m_window(w), m_dock(d), m_announced(d->text()) {}
    ~AccessibleDock() override { releaseItems(); }
    bool isValid() const override { return m_window; }
    QObject* object() const override { return nullptr; }
    QWindow* window() const override { return m_window; }
    QAccessibleInterface* childAt(int x, int y) const override {
        for (QAccessibleInterface* item : items()) if (item->rect().contains(x, y)) return item;
        return nullptr;
    }
    QAccessibleInterface* parent() const override { return QAccessible::queryAccessibleInterface(m_window.data()); }
    QAccessibleInterface* child(int i) const override { return items().value(i); }
    int childCount() const override { return int(items().size()); }
    int indexOfChild(const QAccessibleInterface* c) const override { return int(items().indexOf(const_cast<QAccessibleInterface*>(c))); }
    QString text(QAccessible::Text kind) const override {
        if (kind == QAccessible::Name) return m_dock->text();
        if (kind == QAccessible::Description) return QStringLiteral("Script area");
        return {};
    }
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override { return m_window ? m_window->accessibleDockRect(m_dock) : QRect(); }
    QAccessible::Role role() const override { return QAccessible::StaticText; }
    QAccessible::State state() const override {
        QAccessible::State s;
        s.readOnly = true;
        s.invisible = rect().isEmpty();
        return s;
    }
    TerminalWindow* terminal() const { return m_window; }
    const DockArea* dock() const { return m_dock; }
    /// The dock drew again: tells of a new name, and of items that came,
    /// went or were renamed.
    void update() {
        if (m_built && m_built->generation != m_dock->itemsGeneration()) {
            // The items went with their order; the next query makes new ones.
            releaseItems();
            QAccessibleEvent event(this, QAccessible::ObjectReorder);
            QAccessible::updateAccessibility(&event);
        } else if (m_built) {
            for (qsizetype i = 0; i < m_built->items.size(); ++i) {
                const QString name = m_built->items[i]->text(QAccessible::Name);
                if (name == std::exchange(m_built->names[i], name)) continue;
                QAccessibleEvent event(m_built->items[i], QAccessible::NameChanged);
                QAccessible::updateAccessibility(&event);
            }
        }
        if (m_dock->text() == std::exchange(m_announced, m_dock->text())) return;
        QAccessibleEvent event(this, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    }
private:
    // The item objects of one order of the dock's items.
    struct Built {
        quint64 generation;
        QList<QAccessibleInterface*> items;
        QList<QAccessible::Id> ids;
        QStringList names; ///< as last told
    };
    const QList<QAccessibleInterface*>& items() const {
        if (!m_built || m_built->generation != m_dock->itemsGeneration()) {
            releaseItems();
            Built built{m_dock->itemsGeneration(), {}, {}, {}};
            for (std::size_t i = 0; i < m_dock->items().size(); ++i) {
                auto* item = new AccessibleDockItem(const_cast<AccessibleDock*>(this), i, built.generation);
                built.ids.append(QAccessible::registerAccessibleInterface(item));
                built.items.append(item);
                built.names.append(item->text(QAccessible::Name));
            }
            m_built = std::move(built);
        }
        return m_built->items;
    }
    void releaseItems() const {
        if (!m_built) return;
        const QList<QAccessible::Id> released = std::exchange(m_built, std::nullopt)->ids;
        // The cache owns them once registered, and deletes them by id: at exit
        // it may have deleted them already.
        for (const QAccessible::Id id : released) QAccessible::deleteAccessibleInterface(id);
    }
    QPointer<TerminalWindow> m_window;
    const DockArea* m_dock;
    QString m_announced;
    mutable std::optional<Built> m_built;
};
QWindow* AccessibleDockItem::window() const { return m_dock->window(); }
QAccessibleInterface* AccessibleDockItem::parent() const { return m_dock; }
const DockArea::Item* AccessibleDockItem::item() const
{
    const DockArea* dock = m_dock->dock();
    if (!m_dock->isValid() || dock->itemsGeneration() != m_generation || m_index >= dock->items().size()) return nullptr;
    return &dock->items()[m_index];
}
QRect AccessibleDockItem::rect() const
{
    return item() ? m_dock->terminal()->accessibleDockItemRect(m_dock->dock(), m_index) : QRect();
}
void AccessibleDockItem::doAction(const QString& name)
{
    if (name == pressAction() && item()) m_dock->terminal()->pressDockItem(m_dock->dock(), m_index);
}
QHash<const DockArea*, QAccessible::Id>& dockIds()
{
    static QHash<const DockArea*, QAccessible::Id> ids;
    return ids;
}
QAccessibleInterface* dockInterface(TerminalWindow* w, const DockArea* dock)
{
    auto& ids = dockIds();
    if (const auto it = ids.constFind(dock); it != ids.cend()) return QAccessible::accessibleInterface(*it);
    auto* made = new AccessibleDock(w, dock);
    ids.insert(dock, QAccessible::registerAccessibleInterface(made));
    return made;
}
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
    // The children: the visible panes, then the docks that show, then the chrome.
    QList<QAccessibleInterface*> docks() const {
        QList<QAccessibleInterface*> result;
        for (const DockArea* d : terminal()->accessibleDocks()) result.append(dockInterface(terminal(), d));
        return result;
    }
    QAccessibleInterface* parent() const override { return QAccessible::queryAccessibleInterface(qApp); }
    int childCount() const override { return int(visiblePanes().size() + terminal()->accessibleDocks().size()) + (terminal()->accessibleChrome() ? 1 : 0); }
    QAccessibleInterface* child(int i) const override {
        const auto p = visiblePanes();
        if (i >= 0 && i < p.size()) return QAccessible::queryAccessibleInterface(p[i]);
        const auto d = docks();
        if (i >= p.size() && i < p.size() + d.size()) return d[i - p.size()];
        return i == p.size() + d.size() ? terminal()->accessibleChrome() : nullptr;
    }
    int indexOfChild(const QAccessibleInterface* c) const override {
        if (!c) return -1;
        const auto p = visiblePanes();
        const auto d = docks();
        if (c == terminal()->accessibleChrome()) return int(p.size() + d.size());
        if (const auto at = d.indexOf(const_cast<QAccessibleInterface*>(c)); at >= 0) return int(p.size() + at);
        return int(p.indexOf(qobject_cast<Pane*>(c->object())));
    }
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
void dockAccessibilityChanged(const DockArea* dock)
{
    if (!QAccessible::isActive()) return;
    // Only a dock that something asked about has an object to tell of.
    const auto it = dockIds().constFind(dock);
    if (it == dockIds().cend()) return;
    if (auto* accessible = dynamic_cast<AccessibleDock*>(QAccessible::accessibleInterface(*it))) accessible->update();
}
void releaseDockAccessibility(const DockArea* dock)
{
    if (const QAccessible::Id id = dockIds().take(dock)) QAccessible::deleteAccessibleInterface(id);
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
