// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// Asks for a split's or a tab's title. Enter sets it (empty returns to the
// program's title), Escape or a click outside leaves it as it was.
Item {
    id: prompt

    required property QtObject controller
    readonly property color ink: controller.foreground

    onVisibleChanged: {
        if (visible) {
            input.text = controller.titlePrompt.text || ""
            input.selectAll()
            input.forceActiveFocus()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.35
        MouseArea {
            anchors.fill: parent
            onClicked: prompt.controller.cancelTitle()
        }
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.2
        width: Math.min(420, parent.width * 0.85)
        height: 92
        radius: 8
        color: Qt.lighter(prompt.controller.background, 1.15)
        border.color: Qt.rgba(prompt.ink.r, prompt.ink.g, prompt.ink.b, 0.15)

        Text {
            x: 14
            y: 12
            text: prompt.controller.titlePrompt.heading || ""
            color: prompt.ink
            opacity: 0.6
            font.pixelSize: 12
        }

        Rectangle {
            x: 12
            y: 36
            width: parent.width - 24
            height: 32
            radius: 5
            color: Qt.rgba(0, 0, 0, 0.2)
            border.color: Qt.rgba(prompt.controller.accent.r, prompt.controller.accent.g, prompt.controller.accent.b, 0.6)

            TextInput {
                id: input
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                verticalAlignment: TextInput.AlignVCenter
                color: prompt.ink
                selectionColor: prompt.controller.accent
                font.pixelSize: 14
                maximumLength: 256
                Keys.onReturnPressed: prompt.controller.answerTitle(text)
                Keys.onEnterPressed: prompt.controller.answerTitle(text)
                Keys.onEscapePressed: prompt.controller.cancelTitle()

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: input.text.length === 0
                    text: "Empty follows the program's title"
                    color: prompt.ink
                    opacity: 0.35
                    font: input.font
                }
            }
        }
    }
}
