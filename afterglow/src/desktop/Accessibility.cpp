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
#include <algorithm>
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
// every pane: switching, moving, detaching and closing tabs, closing the
// window, fonts, fullscreen, equalize and rotate, the overview, shaders,
// projects, the configuration and the palette itself. The tab bar and the
// palette reach those, and on each pane they would bury its own. End Script
// Key Mode is the chord that frees the keys, not a pane action.
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
// An action's name is its palette title: Qt's AT-SPI bridge gives the name,
// not the localized name, to a screen reader, as with its own "Show Menu".
// A client asks for every action's keys at once: the lookup is built once.
struct PaneActions {
    QStringList titles; ///< in the palette's order
    QHash<QString, Action> byTitle;
};
const PaneActions& paneActions()
{
    static const PaneActions actions = [] {
        PaneActions result;
        for (Action a : Keybinds::allActions()) {
            if (!paneAction(a)) continue;
            result.titles.append(Keybinds::title(a));
            result.byTitle.insert(Keybinds::title(a), a);
        }
        return result;
    }();
    return actions;
}
Action paneActionNamed(const QString& name)
{
    return paneActions().byTitle.value(name, Action::None);
}
class AccessiblePane final : public QAccessibleObject, public QAccessibleTextInterface, public QAccessibleActionInterface {
public:
    explicit AccessiblePane(Pane* p) : QAccessibleObject(p) {
        m_output = QObject::connect(p, &Pane::contentChanged, p, [this] { m_cached.reset(); });
    }
    ~AccessiblePane() override
    {
        QObject::disconnect(m_output);
        QObject::disconnect(m_overlayOutput);
    }
    Pane* pane() const { return qobject_cast<Pane*>(object()); }
    /// The terminal that has the keyboard: an overlay's, in place of the
    /// pane's or as a popup over it, while one runs over it.
    Pane* shown() const
    {
        Pane* p = pane();
        return p && p->overlay() ? p->overlay()->pane() : p;
    }
    // A query loop asks for each character in turn. One capture serves it until the
    // content changes or the next frame refreshes it.
    TerminalText snapshot() const {
        // An overlay coming or going changes the terminal that shows.
        if (!m_cached || shown() != m_cachedFrom) capture();
        return *m_cached;
    }
    /// The pane left the tree (another tab shows, or zoom hides it). When it
    /// shows again, its text then is where the changes it tells of start.
    void invalidate() { m_cached.reset(); m_seeded = false; }
    QWindow* window() const override { return owner(pane()); }
    // Only while the window lists it among its children.
    QAccessibleInterface* parent() const override { return rect().isEmpty() ? nullptr : QAccessible::queryAccessibleInterface(window()); }
    QAccessibleInterface* child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface*) const override { return -1; }
    // Where its text is: a popup's grid while one runs over the pane. The
    // pane is the popup's then, which has its keyboard, and the pane's own
    // text around it is not read out until the popup ends.
    QRect rect() const override { auto* w = owner(pane()); return w ? w->accessiblePaneRect(shown()) : QRect(); }
    QString text(QAccessible::Text kind) const override {
        if (kind == QAccessible::Name) {
            const QString title = !shown()->title().isEmpty() ? shown()->title() : pane()->title();
            return title.isEmpty() ? QStringLiteral("Terminal") : title;
        }
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
        // A prompt or the palette that holds the keyboard has the focus then.
        s.focused = w && w->isActive() && w->focusedPane() == pane() && !w->chromeHasKeyboard();
        return s;
    }
    void* interface_cast(QAccessible::InterfaceType t) override {
        if (t == QAccessible::TextInterface) return static_cast<QAccessibleTextInterface*>(this);
        if (t == QAccessible::ActionInterface) return static_cast<QAccessibleActionInterface*>(this);
        return nullptr;
    }
    // The menu comes first: a client that runs the first action by default
    // opens the list of the others, not a copy or a split. Focus, which
    // Qt's bridge runs for a client's GrabFocus, comes last.
    QStringList actionNames() const override {
        return QStringList{showMenuAction()} + paneActions().titles + QStringList{setFocusAction()};
    }
    QStringList keyBindingsForAction(const QString& name) const override {
        const Action a = paneActionNamed(name);
        auto* w = owner(pane());
        return a == Action::None || !w ? QStringList() : w->actionShortcuts(a);
    }
    // As the action's keys in this pane: the same confirmations and notices,
    // and nothing while a prompt or a mode holds the keyboard.
    void doAction(const QString& name) override {
        auto* w = owner(pane());
        if (!w) return;
        if (name == showMenuAction()) w->openPaneMenu(pane());
        else if (name == setFocusAction()) { if (w->acceptsAccessibleActions()) w->revealPane(pane()); }
        else if (const Action a = paneActionNamed(name); a != Action::None) w->runAccessibleAction(pane(), a);
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
        shown()->startSelection(s.cells[start], AfterglowVt::SelectionMode::Normal, true);
        shown()->extendSelection(s.cells[end - 1]);
        selectionChanged();
    }
    void removeSelection(int index) override {
        if (index != 0 || snapshot().selectionStart < 0) return;
        shown()->clearSelection();
        selectionChanged();
    }
    QRect characterRect(int offset) const override {
        const auto s = snapshot();
        if (offset < 0 || offset >= s.cells.size()) return {};
        auto* w = owner(pane());
        return w ? w->accessibleCellRect(shown(), s.cells[offset]) : QRect();
    }
    int offsetAtPoint(const QPoint& point) const override {
        auto* w = owner(pane());
        const auto cell = w ? w->accessibleCellAt(shown(), point) : std::nullopt;
        if (!cell) return -1;
        // The cells run in reading order, one for each UTF-16 unit: the
        // first unit of the last cell that starts at or before the point.
        const auto s = snapshot();
        const auto after = std::upper_bound(s.cells.cbegin(), s.cells.cend(), *cell);
        if (after == s.cells.cbegin() || std::prev(after)->row != cell->row) return -1;
        return int(std::lower_bound(s.cells.cbegin(), after, *std::prev(after)) - s.cells.cbegin());
    }
    // The text is the viewport, which shows whenever the pane is in the
    // tree: there is nothing to scroll to, and the focus stays where it is.
    void scrollToSubstring(int, int) override {}
    QString attributes(int offset, int* start, int* end) const override {
        const int count = characterCount();
        if (offset < 0 || offset >= count) { *start = *end = -1; return {}; }
        *start = 0; *end = count; return QStringLiteral("read-only:true;");
    }
    void notify() {
        // Nothing an assistive technology reads changed: no text to rebuild.
        // Serials of two terminals are not comparable: an overlay that comes
        // or goes is a change, whatever they are.
        const Pane* source = shown();
        const auto serial = source->accessibleSerial();
        if (m_seeded && m_notified == serial && m_notifiedFrom == source) return;
        m_notified = serial;
        m_notifiedFrom = source;
        capture();
        const auto next = *m_cached;
        // The first frame after the pane joined the tree: a client reads it
        // whole, and the screen it shows is no change.
        if (!std::exchange(m_seeded, true)) {
            m_previous = next;
            return;
        }
        // One diff for each frame: what the program wrote between two frames
        // is one change.
        for (const auto& edit : textEdits(m_previous.text, next.text)) {
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
        if (next.cursor != m_previous.cursor) {
            QAccessibleTextCursorEvent event(pane(), next.cursor); QAccessible::updateAccessibility(&event);
        }
        if (next.selectionStart != m_previous.selectionStart || next.selectionEnd != m_previous.selectionEnd) {
            QAccessibleTextSelectionEvent event(pane(), next.selectionStart, next.selectionEnd); QAccessible::updateAccessibility(&event);
        }
        m_previous = next;
    }
private:
    /// Takes the text of the terminal that shows. The pane's own text drops
    /// the capture with its contentChanged, and an overlay's with its own
    /// while it shows: a query never takes the terminal's lock to check.
    void capture() const {
        const Pane* source = shown();
        if (source != m_cachedFrom) {
            QObject::disconnect(m_overlayOutput);
            if (source != pane()) m_overlayOutput = QObject::connect(source, &Pane::contentChanged, source, [this] { m_cached.reset(); });
            m_cachedFrom = source;
        }
        m_cached = source->accessibleText();
    }
    // The selection is drawn, and told of, at the next frame.
    void selectionChanged() {
        m_cached.reset();
        if (auto* w = owner(pane())) w->requestUpdate();
    }
    mutable std::optional<TerminalText> m_cached;
    mutable QPointer<const Pane> m_cachedFrom; ///< the terminal m_cached came from
    mutable QMetaObject::Connection m_overlayOutput; ///< drops m_cached when m_cachedFrom is an overlay's
    QPointer<const Pane> m_notifiedFrom; ///< the terminal of m_notified
    TerminalText m_previous; ///< the text as last told of
    bool m_seeded = false; ///< m_previous holds the text the pane shows in the tree
    std::optional<std::pair<std::uint64_t, std::uint64_t>> m_notified;
    QMetaObject::Connection m_output;
};
class AccessibleDock;
// An item of a dock: a button that sends its script the click a left press
// sends. Its dock makes it, and deletes it before the dock goes or when the
// order of the dock's items changes. Until then it stays valid, so that its
// removal reaches clients, and reads the item at its index while that order
// is drawn.
class AccessibleDockItem final : public QAccessibleInterface, public QAccessibleActionInterface {
public:
    AccessibleDockItem(AccessibleDock* dock, std::size_t index, quint64 generation)
        : m_dock(dock), m_index(index), m_generation(generation) {}
    bool isValid() const override;
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
    // Pressed, not focused: the keyboard stays with the panes.
    QAccessible::State state() const override;
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
// window releases them before the dock goes.
class AccessibleDock final : public QAccessibleInterface {
public:
    AccessibleDock(TerminalWindow* w, const DockArea* d)
        : m_window(w), m_dock(d), m_announced(d->text()), m_toolBar(!d->items().empty()), m_rect(rect()) {}
    // Only the cache's own destruction at exit gets here with items left:
    // release() has deleted them otherwise.
    ~AccessibleDock() override { releaseItems(); }
    /// Deletes the item objects while the dock is still registered: their
    /// removal names the dock as their parent, and a dock that the cache
    /// no longer holds would be registered again under a new id.
    void release() { releaseItems(); }
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
    // With buttons to press, a toolbar, which screen readers look inside.
    QAccessible::Role role() const override { return m_dock->items().empty() ? QAccessible::StaticText : QAccessible::ToolBar; }
    QAccessible::State state() const override {
        QAccessible::State s;
        s.readOnly = true;
        s.invisible = !m_window || !m_window->isVisible() || rect().isEmpty();
        return s;
    }
    TerminalWindow* terminal() const { return m_window; }
    const DockArea* dock() const { return m_dock; }
    /// The dock drew again or moved: tells of a new name, role or place,
    /// and of items that came, went, moved or were renamed.
    void update() {
        if (m_built && m_built->generation != m_dock->itemsGeneration()) {
            // The items went with their order; the next query makes new ones.
            releaseItems();
            QAccessibleEvent event(this, QAccessible::ObjectReorder);
            QAccessible::updateAccessibility(&event);
        } else if (m_built) {
            for (qsizetype i = 0; i < m_built->items.size(); ++i) {
                QAccessibleInterface* item = m_built->items[i];
                const QString name = item->text(QAccessible::Name);
                if (name != std::exchange(m_built->names[i], name)) {
                    QAccessibleEvent event(item, QAccessible::NameChanged);
                    QAccessible::updateAccessibility(&event);
                }
                const QRect place = item->rect();
                if (place != std::exchange(m_built->rects[i], place)) {
                    QAccessibleEvent event(item, QAccessible::LocationChanged);
                    QAccessible::updateAccessibility(&event);
                }
            }
        }
        if (const bool toolBar = !m_dock->items().empty(); toolBar != std::exchange(m_toolBar, toolBar)) {
            QAccessibleEvent event(this, QAccessible::RoleChanged);
            QAccessible::updateAccessibility(&event);
        }
        if (const QRect place = rect(); place != std::exchange(m_rect, place)) {
            QAccessibleEvent event(this, QAccessible::LocationChanged);
            QAccessible::updateAccessibility(&event);
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
        QList<QRect> rects; ///< as last told
    };
    // Made on the first query. Only update() replaces them, never a query:
    // a client may hold an item while it asks its dock about another.
    const QList<QAccessibleInterface*>& items() const {
        if (!m_built) {
            Built built{m_dock->itemsGeneration(), {}, {}, {}, {}};
            for (std::size_t i = 0; i < m_dock->items().size(); ++i) {
                auto* item = new AccessibleDockItem(const_cast<AccessibleDock*>(this), i, built.generation);
                built.ids.append(QAccessible::registerAccessibleInterface(item));
                built.items.append(item);
                built.names.append(item->text(QAccessible::Name));
                built.rects.append(item->rect());
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
    bool m_toolBar; ///< the role as last told
    QRect m_rect; ///< the place as last told
    mutable std::optional<Built> m_built;
};
bool AccessibleDockItem::isValid() const { return m_dock->isValid(); }
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
QAccessible::State AccessibleDockItem::state() const
{
    QAccessible::State s;
    auto* w = m_dock->terminal();
    s.invisible = !w || !w->isVisible() || rect().isEmpty();
    return s;
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
    explicit AccessibleWindow(TerminalWindow* w) : QAccessibleObject(w), m_children(childObjects()) {}
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
    QList<QAccessibleInterface*> children() const {
        QList<QAccessibleInterface*> result;
        for (Pane* p : visiblePanes()) result.append(QAccessible::queryAccessibleInterface(p));
        result.append(docks());
        if (auto* chrome = terminal()->accessibleChrome()) result.append(chrome);
        return result;
    }
    /// Tells clients to read the children again when the panes or docks
    /// they are made of changed: a split, a zoom, a tab switch, a swap.
    void syncChildren() {
        if (childObjects() == m_children) return;
        m_children = childObjects();
        QAccessibleEvent event(terminal(), QAccessible::ObjectReorder);
        QAccessible::updateAccessibility(&event);
    }
    QAccessibleInterface* parent() const override { return QAccessible::queryAccessibleInterface(qApp); }
    int childCount() const override { return int(visiblePanes().size() + terminal()->accessibleDocks().size()) + (terminal()->accessibleChrome() ? 1 : 0); }
    QAccessibleInterface* child(int i) const override { return children().value(i); }
    int indexOfChild(const QAccessibleInterface* c) const override {
        return c ? int(children().indexOf(const_cast<QAccessibleInterface*>(c))) : -1;
    }
    QAccessibleInterface* childAt(int x, int y) const override {
        if (auto* chrome = terminal()->accessibleChrome()) if (auto* control = chrome->childAt(x, y)) return control;
        for (QAccessibleInterface* c : children()) if (c && c->rect().contains(x, y)) return c;
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
private:
    /// What the children are, in order: by id, which a new pane or dock at a
    /// freed one's address does not share. A dock's id is its owner's, and
    /// each pane and the window can have one with the same id: its pane
    /// tells them apart. The chrome comes once it is made.
    QStringList childObjects() const {
        QStringList result;
        for (Pane* p : visiblePanes()) result.append(QStringLiteral("pane %1").arg(p->id()));
        for (const DockArea* d : terminal()->accessibleDocks())
            result.append(QStringLiteral("dock %1 %2 %3").arg(d->owner(), d->id()).arg(d->pane()));
        if (terminal()->accessibleChrome()) result.append(QStringLiteral("chrome"));
        return result;
    }
    QStringList m_children; ///< as clients last heard
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
    // Only a dock that something asked about has an object to tell of. Its
    // items follow each drawing, heard or not, so that no query finds them
    // out of date.
    const auto it = dockIds().constFind(dock);
    if (it == dockIds().cend()) return;
    if (auto* accessible = dynamic_cast<AccessibleDock*>(QAccessible::accessibleInterface(*it))) accessible->update();
}
void releaseDockAccessibility(const DockArea* dock)
{
    const QAccessible::Id id = dockIds().take(dock);
    if (!id) return;
    if (auto* accessible = dynamic_cast<AccessibleDock*>(QAccessible::accessibleInterface(id))) accessible->release();
    QAccessible::deleteAccessibleInterface(id);
}
void windowAccessibilityChanged(TerminalWindow* w)
{
    if (!QAccessible::isActive()) return;
    if (auto* accessible = dynamic_cast<AccessibleWindow*>(QAccessible::queryAccessibleInterface(w))) accessible->syncChildren();
}
void updateTerminalAccessibility(TerminalWindow* w)
{
    if (!QAccessible::isActive()) return;
    windowAccessibilityChanged(w);
    for (auto* p : w->panes()) {
        auto* a = dynamic_cast<AccessiblePane*>(QAccessible::queryAccessibleInterface(p));
        if (!a) continue;
        // Background tabs and panes hidden by zoom are not in the tree and send no events.
        if (w->tabIdOf(p) == w->activeTabId() && !w->accessiblePaneRect(p).isEmpty()) a->notify();
        else a->invalidate();
    }
}
}
