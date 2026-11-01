// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC

// The right-click menu, with submenus. Each open panel is a level: the menu
// itself, then a submenu beside the row that opened it, and so on. Panels
// keep inside the window, flipping left or up near an edge, and a panel
// taller than the window scrolls. Clicking outside closes everything; the
// keyboard works on the innermost panel.
//
// Items: { id, title, shortcut, icon, enabled, checked, items (a submenu) }
// or { separator: true }.
Item {
    id: menu

    required property QtObject controller

    // Open panels, outermost first: { items, left, right, top, root }.
    property var levels: []
    // The highlighted row of each level, -1 for none.
    property var currents: []
    // How long the pointer rests on a submenu row before it opens.
    readonly property int submenuDelay: 160

    // One entry per open level. The panels follow this model, so opening or
    // closing a submenu adds or removes one panel and keeps the others (with
    // their rows and hover state) as they are.
    ListModel { id: panelModel }
    onLevelsChanged: {
        while (panelModel.count > levels.length)
            panelModel.remove(panelModel.count - 1)
        while (panelModel.count < levels.length)
            panelModel.append({})
    }

    function syncVisibility() {
        if (visible) {
            const at = controller.contextMenu
            levels = [{ items: at.items || [], left: at.x || 0, right: at.x || 0, top: at.y || 0, root: true }]
            currents = [-1]
            hoverTimer.stop()
        } else {
            levels = []
            currents = []
        }
    }

    onVisibleChanged: syncVisibility()
    Component.onCompleted: syncVisibility()

    function takeFocus() {
        keys.forceActiveFocus()
    }

    function itemAt(level: int, index: int): var {
        return level < levels.length ? levels[level].items[index] : undefined
    }

    function selectable(level: int, index: int): bool {
        const item = itemAt(level, index)
        return item !== undefined && !item.separator && item.enabled !== false
    }

    function setCurrent(level: int, index: int) {
        const c = currents.slice()
        c[level] = index
        currents = c
    }

    function closeFrom(level: int) {
        if (level < levels.length) {
            levels = levels.slice(0, level)
            currents = currents.slice(0, level)
        }
    }

    // Opens the submenu of row @p index at @p level beside that row.
    function openSubmenu(level: int, index: int, highlightFirst: bool) {
        const item = itemAt(level, index)
        const panel = panels.itemAt(level)
        if (!item || !item.items || !panel)
            return
        const row = panel.rowAt(index)
        const top = row ? row.mapToItem(menu, 0, 0).y - 5 : panel.y
        const lv = levels.slice(0, level + 1)
        lv.push({ items: item.items, left: panel.x + 4, right: panel.x + panel.width - 4, top: top, root: false })
        const c = currents.slice(0, level + 1)
        c[level] = index
        c.push(-1)
        levels = lv
        currents = c
        if (highlightFirst)
            step(level + 1, 1)
    }

    function step(level: int, direction: int) {
        const items = level < levels.length ? levels[level].items : []
        if (items.length === 0)
            return
        let i = currents[level]
        if (i < 0 && direction < 0)
            i = 0
        for (let n = 0; n < items.length; ++n) {
            i = (i + direction + items.length) % items.length
            if (selectable(level, i)) {
                setCurrent(level, i)
                const panel = panels.itemAt(level)
                if (panel)
                    panel.reveal(i)
                return
            }
        }
    }

    function activate(level: int, index: int) {
        if (!selectable(level, index))
            return
        const item = itemAt(level, index)
        if (item.items)
            openSubmenu(level, index, true)
        else
            controller.activateMenuItem(item.id)
    }

    // Hovering a submenu row opens it after a moment, so passing over rows
    // on the way to another does not flicker panels open and shut.
    Timer {
        id: hoverTimer
        property int level: -1
        property int index: -1
        interval: menu.submenuDelay
        onTriggered: {
            const item = menu.itemAt(level, index)
            if (item && item.items && menu.selectable(level, index))
                menu.openSubmenu(level, index, false)
            else
                menu.closeFrom(level + 1)
        }
    }

    QQC.Label { id: menuFont; visible: false }

    FontMetrics {
        id: metrics
        font: menuFont.font
    }

    // Anything outside the panels closes the menu, whatever the button.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: menu.controller.closeContextMenu()
    }

    Repeater {
        id: panels
        model: panelModel

        delegate: QQC.Pane {
            id: panel

            required property int index
            readonly property int level: index
            readonly property var spec: menu.levels[level] || ({ items: [], left: 0, right: 0, top: 0, root: true })
            readonly property real wanted: {
                let w = 0
                for (const item of spec.items) {
                    if (item.separator)
                        continue
                    const trailing = item.items ? 20 : (item.shortcut ? metrics.advanceWidth(item.shortcut) + 36 : 0)
                    w = Math.max(w, metrics.advanceWidth(item.title) + trailing)
                }
                return w + 58 // padding and the icon column
            }

            function rowAt(i: int): Item {
                return rows.itemAt(i)
            }

            // Scrolls row @p i into view when the panel is shorter than its rows.
            function reveal(i: int) {
                const row = rows.itemAt(i)
                if (!row)
                    return
                if (row.y < scroller.contentY)
                    scroller.contentY = row.y
                else if (row.y + row.height > scroller.contentY + scroller.height)
                    scroller.contentY = row.y + row.height - scroller.height
            }

            z: level
            width: Math.min(Math.max(spec.root ? 220 : 180, wanted), menu.width - 8)
            height: Math.min(column.implicitHeight + 10, menu.height - 8)
            x: spec.right + width + 4 <= menu.width ? spec.right : Math.max(4, spec.left - width)
            y: spec.root ? (spec.top + height + 4 <= menu.height ? spec.top : Math.max(4, spec.top - height))
                         : Math.max(4, Math.min(spec.top, menu.height - height - 4))
            padding: 5

            // Presses between rows must not close the menu.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
            }

            Flickable {
                id: scroller
                anchors.fill: parent
                contentWidth: width
                contentHeight: column.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentHeight > height
                QQC.ScrollBar.vertical: QQC.ScrollBar {}

                Column {
                    id: column
                    width: scroller.width

                    Repeater {
                        id: rows
                        model: panel.spec.items
                        // Another submenu in this panel starts at its top,
                        // not at the scroll of the one it replaced.
                        onModelChanged: scroller.contentY = 0

                        delegate: Item {
                            id: entry

                            required property int index
                            required property var modelData
                            readonly property bool separator: modelData.separator === true
                            readonly property bool usable: !separator && modelData.enabled !== false
                            readonly property bool submenu: !separator && modelData.items !== undefined
                            readonly property bool current: menu.currents[panel.level] === index

                            width: column.width
                            height: separator ? divider.implicitHeight : menuItem.implicitHeight

                            QQC.MenuSeparator {
                                id: divider
                                visible: entry.separator
                                width: parent.width
                            }

                            QQC.MenuItem {
                                id: menuItem
                                visible: !entry.separator
                                width: parent.width
                                enabled: entry.usable
                                highlighted: entry.current && entry.usable
                                focusPolicy: Qt.NoFocus
                                // Keep the native icon/check layout while treating titles literally.
                                // The label drops "&" as a mnemonic marker before the rich
                                // text step, so each entity carries an extra "&" for it.
                                readonly property string literalTitle: entry.modelData.title || ""
                                text: "<span>" + literalTitle.replace(/&/g, "&&amp;").replace(/</g, "&&lt;").replace(/>/g, "&&gt;") + "</span>"
                                Accessible.name: literalTitle
                                icon.name: entry.modelData.icon || ""
                                checkable: entry.modelData.checked !== undefined
                                checked: entry.modelData.checked === true
                                rightPadding: shortcut.implicitWidth + 24
                                Accessible.description: entry.modelData.shortcut || ""
                                onTriggered: menu.activate(panel.level, entry.index)

                                QQC.Label {
                                    id: shortcut
                                    anchors.right: parent.right
                                    anchors.rightMargin: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: entry.submenu ? "›" : (entry.modelData.shortcut || "")
                                    textFormat: Text.PlainText
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                enabled: !entry.separator
                                hoverEnabled: true
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onEntered: {
                                    menu.setCurrent(panel.level, entry.usable ? entry.index : -1)
                                    hoverTimer.level = panel.level
                                    hoverTimer.index = entry.index
                                    hoverTimer.restart()
                                }
                                onClicked: {
                                    hoverTimer.stop()
                                    menu.activate(panel.level, entry.index)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Item {
        id: keys
        readonly property int level: menu.levels.length - 1
        Keys.onUpPressed: menu.step(level, -1)
        Keys.onDownPressed: menu.step(level, 1)
        Keys.onRightPressed: {
            const item = menu.itemAt(level, menu.currents[level])
            if (item && item.items && menu.selectable(level, menu.currents[level]))
                menu.openSubmenu(level, menu.currents[level], true)
        }
        Keys.onLeftPressed: {
            if (level > 0)
                menu.closeFrom(level)
        }
        Keys.onReturnPressed: menu.activate(level, menu.currents[level])
        Keys.onEnterPressed: menu.activate(level, menu.currents[level])
        Keys.onSpacePressed: menu.activate(level, menu.currents[level])
        Keys.onEscapePressed: {
            if (level > 0)
                menu.closeFrom(level)
            else
                menu.controller.closeContextMenu()
        }
    }
}
