// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// The right-click menu, with submenus. Each open panel is a level: the menu
// itself, then a submenu beside the row that opened it, and so on. Panels
// keep inside the window, flipping left or up near an edge. Clicking outside
// closes everything; the keyboard works on the innermost panel.
//
// Items: { id, title, shortcut, icon, enabled, checked, items (a submenu) }
// or { separator: true }.
Item {
    id: menu

    required property QtObject controller

    readonly property color ink: controller.foreground
    readonly property string inkName: ink.toString()
    // Open panels, outermost first: { items, left, right, top, root }.
    property var levels: []
    // The highlighted row of each level, -1 for none.
    property var currents: []

    onVisibleChanged: {
        if (visible) {
            const at = controller.contextMenu
            levels = [{ items: at.items || [], left: at.x || 0, right: at.x || 0, top: at.y || 0, root: true }]
            currents = [-1]
            hoverTimer.stop()
            keys.forceActiveFocus()
        } else {
            levels = []
            currents = []
        }
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
        for (let n = 0; n < items.length; ++n) {
            i = (i + direction + items.length) % items.length
            if (selectable(level, i)) {
                setCurrent(level, i)
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
        interval: 160
        onTriggered: {
            const item = menu.itemAt(level, index)
            if (item && item.items && menu.selectable(level, index))
                menu.openSubmenu(level, index, false)
            else
                menu.closeFrom(level + 1)
        }
    }

    FontMetrics {
        id: metrics
        font.pixelSize: 13
    }

    // Anything outside the panels closes the menu, whatever the button.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: menu.controller.closeContextMenu()
    }

    Repeater {
        id: panels
        model: menu.levels.length

        delegate: Rectangle {
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

            z: level
            width: Math.min(Math.max(spec.root ? 220 : 180, wanted), menu.width - 8)
            height: column.implicitHeight + 10
            x: spec.right + width + 4 <= menu.width ? spec.right : Math.max(4, spec.left - width)
            y: spec.root ? (spec.top + height + 4 <= menu.height ? spec.top : Math.max(4, spec.top - height))
                         : Math.max(4, Math.min(spec.top, menu.height - height - 4))
            radius: 8
            color: Qt.lighter(menu.controller.background, 1.15)
            border.color: Qt.rgba(menu.ink.r, menu.ink.g, menu.ink.b, 0.15)

            // A soft shadow, drawn behind.
            Rectangle {
                z: -1
                anchors.fill: parent
                anchors.margins: -3
                anchors.topMargin: -1
                anchors.bottomMargin: -6
                radius: parent.radius + 3
                color: "#000000"
                opacity: 0.25
            }

            // Presses between rows must not close the menu.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
            }

            Column {
                id: column
                x: 5
                y: 5
                width: parent.width - 10

                Repeater {
                    id: rows
                    model: panel.spec.items

                    delegate: Item {
                        id: entry

                        required property int index
                        required property var modelData
                        readonly property bool separator: modelData.separator === true
                        readonly property bool usable: !separator && modelData.enabled !== false
                        readonly property bool submenu: !separator && modelData.items !== undefined
                        readonly property bool current: menu.currents[panel.level] === index

                        width: column.width
                        height: separator ? 9 : 28

                        Rectangle {
                            visible: entry.separator
                            anchors.verticalCenter: parent.verticalCenter
                            x: 6
                            width: parent.width - 12
                            height: 1
                            color: Qt.rgba(menu.ink.r, menu.ink.g, menu.ink.b, 0.1)
                        }

                        Rectangle {
                            visible: !entry.separator
                            anchors.fill: parent
                            radius: 5
                            color: entry.current && entry.usable
                                   ? Qt.rgba(menu.controller.accent.r, menu.controller.accent.g, menu.controller.accent.b, 0.25)
                                   : "transparent"
                            opacity: entry.usable ? 1.0 : 0.38

                            // The icon column: a check for toggles that are on, else the icon.
                            Text {
                                visible: entry.modelData.checked === true
                                x: 9
                                anchors.verticalCenter: parent.verticalCenter
                                text: "✓"
                                color: menu.controller.accent
                                font.pixelSize: 14
                                font.bold: true
                            }
                            Image {
                                visible: entry.modelData.checked !== true && status === Image.Ready
                                x: 9
                                anchors.verticalCenter: parent.verticalCenter
                                width: 16
                                height: 16
                                sourceSize: Qt.size(32, 32)
                                source: entry.modelData.icon ? "image://icon/" + entry.modelData.icon + "/" + menu.inkName : ""
                                smooth: true
                                opacity: 0.85
                            }
                            Text {
                                x: 34
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - x - (entry.submenu ? 24 : shortcut.implicitWidth + 20)
                                text: entry.modelData.title || ""
                                elide: Text.ElideRight
                                color: menu.ink
                                opacity: 0.95
                                font.pixelSize: metrics.font.pixelSize
                            }
                            Text {
                                id: shortcut
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: entry.submenu ? "›" : (entry.modelData.shortcut || "")
                                color: menu.ink
                                opacity: entry.submenu ? 0.7 : 0.5
                                font.pixelSize: entry.submenu ? 16 : 12
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

    Item {
        id: keys
        focus: true
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
