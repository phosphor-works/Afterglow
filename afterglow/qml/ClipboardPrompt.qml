// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// Asks before a program reads the clipboard (OSC 5522). A program that sent
// a name and password can be allowed for the rest of the session.
Item {
    id: prompt

    required property QtObject controller
    readonly property bool canRemember: controller.clipboardPromptName.length > 0

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
            onClicked: prompt.controller.answerClipboard(0)
        }
    }

    component Choice: Rectangle {
        id: choice
        property string label
        property int answer
        property bool primary: false
        width: text.implicitWidth + 24
        height: text.implicitHeight + 12
        radius: 4
        color: primary ? prompt.controller.accent
                       : (area.containsMouse ? Qt.lighter(prompt.controller.background, 1.4)
                                             : Qt.lighter(prompt.controller.background, 1.25))
        Text {
            id: text
            anchors.centerIn: parent
            text: choice.label
            color: choice.primary ? prompt.controller.background : prompt.controller.foreground
            font.pixelSize: 13
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: prompt.controller.answerClipboard(choice.answer)
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(560, parent.width * 0.85)
        height: column.implicitHeight + 32
        radius: 8
        color: Qt.lighter(prompt.controller.background, 1.15)
        border.color: Qt.rgba(prompt.controller.foreground.r, prompt.controller.foreground.g,
                              prompt.controller.foreground.b, 0.15)

        Column {
            id: column
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            Text {
                width: parent.width
                text: "Allow clipboard access?"
                color: prompt.controller.foreground
                font.pixelSize: 15
                font.bold: true
            }
            Text {
                width: parent.width
                text: prompt.controller.clipboardPromptText
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: prompt.controller.foreground
                opacity: 0.8
                font.pixelSize: 13
            }
            Row {
                spacing: 8
                Choice { label: "Allow once"; answer: 1; primary: true }
                Choice {
                    visible: prompt.canRemember
                    label: "Always allow " + prompt.controller.clipboardPromptName
                    answer: 2
                }
                Choice { label: "Deny"; answer: 0 }
            }
            Text {
                text: prompt.canRemember ? "Enter to allow once, A to always allow, Escape to deny"
                                         : "Enter to allow once, Escape to deny"
                color: prompt.controller.foreground
                opacity: 0.5
                font.pixelSize: 12
            }
        }
    }

    Item {
        id: keys
        focus: true
        Keys.onReturnPressed: prompt.controller.answerClipboard(1)
        Keys.onEnterPressed: prompt.controller.answerClipboard(1)
        Keys.onEscapePressed: prompt.controller.answerClipboard(0)
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_A && prompt.canRemember) {
                prompt.controller.answerClipboard(2)
                event.accepted = true
            }
        }
    }
}
