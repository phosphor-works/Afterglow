// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: bar

    required property QtObject controller

    width: Math.min(500, parent ? parent.width - 16 : 500)
    height: implicitHeight
    padding: 6
    Keys.onEscapePressed: bar.controller.searchOpen = false

    onVisibleChanged: {
        if (visible) {
            query.selectAll()
            Qt.callLater(() => { if (bar.visible) query.forceActiveFocus() })
        }
    }

    RowLayout {
        width: parent.width
        spacing: 4

        TextField {
            id: query
            focus: bar.visible
            Layout.fillWidth: true
            placeholderText: qsTr("Search")
            Accessible.name: placeholderText
            selectByMouse: true
            onTextChanged: bar.controller.search(text, 0)
            Keys.onEscapePressed: bar.controller.searchOpen = false
            Keys.onReturnPressed: event => bar.controller.search(text, (event.modifiers & Qt.ShiftModifier) ? -1 : 1)
            Keys.onEnterPressed: event => bar.controller.search(text, (event.modifiers & Qt.ShiftModifier) ? -1 : 1)
        }

        ToolButton {
            text: qsTr(".*")
            checkable: true
            checked: bar.controller.searchRegex
            Accessible.name: qsTr("Regular expression")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: {
                bar.controller.searchRegex = checked
                bar.controller.search(query.text, 0)
            }
        }

        Label {
            text: bar.controller.searchStatus
        }

        ToolButton {
            icon.name: "go-up"
            text: qsTr("Previous match")
            display: AbstractButton.IconOnly
            onClicked: bar.controller.search(query.text, -1)
            ToolTip.visible: hovered
            ToolTip.text: text
        }
        ToolButton {
            icon.name: "go-down"
            text: qsTr("Next match")
            display: AbstractButton.IconOnly
            onClicked: bar.controller.search(query.text, 1)
            ToolTip.visible: hovered
            ToolTip.text: text
        }
        ToolButton {
            icon.name: "dialog-close"
            text: qsTr("Close search")
            display: AbstractButton.IconOnly
            onClicked: bar.controller.searchOpen = false
            ToolTip.visible: hovered
            ToolTip.text: text
        }
    }
}
