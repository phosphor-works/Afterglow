// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

Rectangle {
    id: bar

    required property QtObject controller

    width: 360
    height: 38
    radius: 6
    color: Qt.lighter(controller.background, 1.15)
    border.color: Qt.rgba(controller.foreground.r, controller.foreground.g, controller.foreground.b, 0.15)

    onVisibleChanged: {
        if (visible) {
            query.selectAll()
            query.forceActiveFocus()
        }
    }

    TextInput {
        id: query
        anchors.left: parent.left
        anchors.right: status.left
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 12
        anchors.rightMargin: 8
        color: bar.controller.foreground
        selectionColor: bar.controller.accent
        font.pixelSize: 14
        clip: true
        onTextChanged: bar.controller.search(text, 0)
        Keys.onEscapePressed: bar.controller.searchOpen = false
        Keys.onReturnPressed: event => bar.controller.search(text, (event.modifiers & Qt.ShiftModifier) ? -1 : 1)
        Keys.onEnterPressed: event => bar.controller.search(text, (event.modifiers & Qt.ShiftModifier) ? -1 : 1)

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: query.text.length === 0
            text: "Search"
            color: bar.controller.foreground
            opacity: 0.4
            font: query.font
        }
    }

    Text {
        id: status
        anchors.right: buttons.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        text: bar.controller.searchStatus
        color: bar.controller.foreground
        opacity: 0.6
        font.pixelSize: 12
    }

    Row {
        id: buttons
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Repeater {
            model: [
                { label: "↑", direction: -1 },
                { label: "↓", direction: 1 },
                { label: "×", direction: 2 }
            ]
            delegate: Text {
                required property var modelData
                width: 24
                horizontalAlignment: Text.AlignHCenter
                text: modelData.label
                color: bar.controller.foreground
                opacity: area.containsMouse ? 1.0 : 0.6
                font.pixelSize: 15
                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        if (parent.modelData.direction === 2)
                            bar.controller.searchOpen = false
                        else
                            bar.controller.search(query.text, parent.modelData.direction)
                    }
                }
            }
        }
    }
}
