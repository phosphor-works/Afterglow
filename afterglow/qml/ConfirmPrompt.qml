// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// Asks before doing something that cannot be taken back, such as closing a
// tab where a program still runs. Enter or the button goes ahead; Escape, a
// click outside or Cancel leaves things as they are.
Item {
    id: prompt

    required property QtObject controller
    readonly property color ink: controller.foreground
    readonly property var spec: controller.confirmPrompt

    onVisibleChanged: {
        if (visible)
            keys.forceActiveFocus()
    }

    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.35
        MouseArea {
            anchors.fill: parent
            onClicked: prompt.controller.answerConfirm(false)
        }
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.2
        width: Math.min(440, parent.width * 0.85)
        height: column.implicitHeight + 32
        radius: 8
        color: Qt.lighter(prompt.controller.background, 1.15)
        border.color: Qt.rgba(prompt.ink.r, prompt.ink.g, prompt.ink.b, 0.15)

        // Presses on the panel must not count as outside it.
        MouseArea {
            anchors.fill: parent
        }

        Column {
            id: column
            x: 16
            y: 16
            width: parent.width - 32
            spacing: 10

            Text {
                width: parent.width
                text: prompt.spec.heading || ""
                wrapMode: Text.WordWrap
                color: prompt.ink
                font.pixelSize: 15
                font.bold: true
            }
            Text {
                width: parent.width
                visible: text.length > 0
                text: prompt.spec.body || ""
                wrapMode: Text.WordWrap
                color: prompt.ink
                opacity: 0.7
                font.pixelSize: 13
            }
            Row {
                anchors.right: parent.right
                spacing: 8

                Repeater {
                    model: [{ label: "Cancel", accept: false }, { label: prompt.spec.accept || "OK", accept: true }]

                    delegate: Rectangle {
                        required property var modelData
                        width: label.implicitWidth + 28
                        height: 30
                        radius: 5
                        readonly property color tint: modelData.accept ? prompt.controller.badColor : prompt.ink
                        color: area.containsMouse ? Qt.rgba(tint.r, tint.g, tint.b, 0.22) : Qt.rgba(tint.r, tint.g, tint.b, 0.1)
                        border.color: Qt.rgba(tint.r, tint.g, tint.b, modelData.accept ? 0.6 : 0.2)

                        Text {
                            id: label
                            anchors.centerIn: parent
                            text: parent.modelData.label
                            color: parent.modelData.accept ? prompt.controller.badColor : prompt.ink
                            font.pixelSize: 13
                        }
                        MouseArea {
                            id: area
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: prompt.controller.answerConfirm(parent.modelData.accept)
                        }
                    }
                }
            }
        }
    }

    Item {
        id: keys
        focus: true
        Keys.onReturnPressed: prompt.controller.answerConfirm(true)
        Keys.onEnterPressed: prompt.controller.answerConfirm(true)
        Keys.onEscapePressed: prompt.controller.answerConfirm(false)
    }
}
