// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PaneDialog.h"
#include "Pane.h"

#include <QApplication>
#include <QDialog>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QWindow>

#include <algorithm>

namespace Afterglow {

namespace {

/// The window that shows @p pane: the one that shows its host, for an
/// overlay's program. A pane in a tab is a child of its window.
Pane* shownPane(Pane* pane)
{
    return pane && pane->overlayHost() ? pane->overlayHost() : pane;
}

QWindow* windowOf(Pane* pane)
{
    Pane* const shown = shownPane(pane);
    return shown ? qobject_cast<QWindow*>(shown->parent()) : nullptr;
}

/// Moves a dialog to its pane's window before the window it shows over goes.
/// The Wayland platform window closes every window transient for it in its
/// destructor (a queued close event to each), so QObject::destroyed is too
/// late: the close is posted by then. SurfaceAboutToBeDestroyed comes just
/// before, from close() and from ~QWindow alike.
class DialogKeeper final : public QObject
{
public:
    DialogKeeper(QDialog* dialog, Pane* pane) : QObject(dialog), m_dialog(dialog), m_pane(pane) {}

    void attach(QWindow* window)
    {
        QWindow* const own = m_dialog->windowHandle();
        if (m_dialog->isVisible() && !own->isVisible() && nativeDialogOver(own->transientParent())) {
            // A native dialog of the platform theme (the KDE file dialog)
            // shows in a window of its own, made transient at show time for
            // the transient parent of this hidden one, and a later change
            // does not reach it. Shown again, it takes the new window. The
            // native dialog stays between the two, with what the user typed
            // or chose in it: only its surface goes.
            m_dialog->hide();
            own->setTransientParent(window);
            if (m_pane) Q_EMIT shownPane(m_pane)->dialogOpened(m_dialog);
            m_dialog->show();
        } else {
            // The dialog's own window takes the new one. A native dialog
            // that is no window of Qt (the GTK file dialog, or the desktop
            // portal's in another process) stays where it is: the window
            // that closes does not close it. Shown again, the GTK dialog
            // would take back the first name, and the portal would open a
            // second chooser.
            own->setTransientParent(window);
            if (m_pane) Q_EMIT shownPane(m_pane)->dialogOpened(m_dialog);
        }
        window->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* e) override
    {
        if (e->type() != QEvent::PlatformSurface
            || static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() != QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            return false;
        watched->removeEventFilter(this);
        if (QWindow* next = windowOf(m_pane); next && next != watched) attach(next);
        else m_dialog->reject();
        return false;
    }

private:
    /// The window of a native dialog over @p window: a dialog that no
    /// keeper moves, so the platform theme made it for this one.
    QWidget* nativeDialogOver(const QWindow* window) const
    {
        const auto kept = [](const QWidget* widget) {
            const QObjectList& children = widget->children();
            return std::any_of(children.begin(), children.end(), [](QObject* child) { return dynamic_cast<DialogKeeper*>(child); });
        };
        for (QWidget* widget : QApplication::topLevelWidgets())
            if (widget != m_dialog && widget->isVisible() && qobject_cast<QDialog*>(widget) && widget->windowHandle()
                && widget->windowHandle()->transientParent() == window && !kept(widget))
                return widget;
        return nullptr;
    }

    QDialog* m_dialog;
    QPointer<Pane> m_pane;
};

} // namespace

void keepDialogOverPane(QDialog* dialog, Pane* pane)
{
    QWindow* const window = windowOf(pane);
    if (!window) return;
    dialog->winId();
    (new DialogKeeper(dialog, pane))->attach(window);
}

} // namespace Afterglow
