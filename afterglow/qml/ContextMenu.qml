// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// The right-click menu. Opens where it was asked for, moved or flipped to
// stay inside the window. Clicking outside closes it; the keyboard moves
// through the enabled items.
Item {
    id: menu

    required property QtObject controller

    readonly property var items: controller.contextMenu.items || []
    property int current: -1
    readonly property color ink: controller.foreground

    onVisibleChanged: {
        if (visible) {
            current = -1
            keys.forceActiveFocus()
        }
    }

    function selectable(index: int): bool {
        const item = items[index]
        return item !== undefined && !item.separator && item.enabled !== false
    }

    function step(direction: int) {
        if (items.length === 0)
            return
        let i = current
        for (let n = 0; n < items.length; ++n) {
            i = (i + direction + items.length) % items.length
            if (selectable(i)) {
                current = i
                return
            }
        }
    }

    function activate(index: int) {
        if (selectable(index))
            controller.activateMenuItem(items[index].id)
    }

    FontMetrics {
        id: metrics
        font.pixelSize: 13
    }

    // Anything outside the panel closes the menu, whatever the button.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: menu.controller.closeContextMenu()
    }

    Rectangle {
        id: panel

        readonly property real wanted: {
            let w = 0
            for (const item of menu.items) {
                if (!item.separator)
                    w = Math.max(w, metrics.advanceWidth(item.title) + (item.shortcut ? metrics.advanceWidth(item.shortcut) + 36 : 0))
            }
            return w + 32
        }
        readonly property real atX: menu.controller.contextMenu.x || 0
        readonly property real atY: menu.controller.contextMenu.y || 0

        width: Math.min(Math.max(200, wanted), menu.width - 8)
        height: column.implicitHeight + 10
        x: atX + width + 4 <= menu.width ? atX : Math.max(4, atX - width)
        y: atY + height + 4 <= menu.height ? atY : Math.max(4, atY - height)
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

        // Swallow presses between items so they do not close the menu.
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
                model: menu.items
                delegate: Item {
                    id: entry

                    required property int index
                    required property var modelData
                    readonly property bool separator: modelData.separator === true
                    readonly property bool enabled: !separator && modelData.enabled !== false

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
                        color: menu.current === entry.index && entry.enabled
                               ? Qt.rgba(menu.controller.accent.r, menu.controller.accent.g, menu.controller.accent.b, 0.25)
                               : "transparent"

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: entry.modelData.title || ""
                            color: menu.ink
                            opacity: entry.enabled ? 0.95 : 0.35
                            font.pixelSize: metrics.font.pixelSize
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: entry.modelData.shortcut || ""
                            color: menu.ink
                            opacity: entry.enabled ? 0.5 : 0.25
                            font.pixelSize: 12
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: entry.enabled
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onEntered: menu.current = entry.index
                        onExited: if (menu.current === entry.index) menu.current = -1
                        onClicked: menu.activate(entry.index)
                    }
                }
            }
        }
    }

    Item {
        id: keys
        focus: true
        Keys.onEscapePressed: menu.controller.closeContextMenu()
        Keys.onUpPressed: menu.step(-1)
        Keys.onDownPressed: menu.step(1)
        Keys.onReturnPressed: menu.activate(menu.current)
        Keys.onEnterPressed: menu.activate(menu.current)
    }
}
