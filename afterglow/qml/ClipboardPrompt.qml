// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asks before a program reads the clipboard (OSC 5522).
Item {
    id: prompt

    required property QtObject controller
    readonly property bool canRemember: controller.clipboardPromptName.length > 0

    onVisibleChanged: {
        if (visible)
            panel.forceActiveFocus()
    }

    Rectangle {
        anchors.fill: parent
        color: "#59000000"
        MouseArea {
            anchors.fill: parent
            onClicked: prompt.controller.answerClipboard(0)
        }
    }

    Pane {
        id: panel
        anchors.centerIn: parent
        width: Math.min(620, parent.width * 0.85)
        padding: 16

        ColumnLayout {
            width: parent.width
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: qsTr("Allow clipboard access?")
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                text: prompt.controller.clipboardPromptText
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                Button {
                    text: qsTr("Allow once")
                    highlighted: true
                    onClicked: prompt.controller.answerClipboard(1)
                }
                Button {
                    visible: prompt.canRemember
                    width: Math.min(implicitWidth, parent.width)
                    text: qsTr("Always allow %1").arg(prompt.controller.clipboardPromptName)
                    onClicked: prompt.controller.answerClipboard(2)
                }
                Button {
                    text: qsTr("Deny")
                    onClicked: prompt.controller.answerClipboard(0)
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: prompt.canRemember ? qsTr("Enter to allow once, A to always allow, Escape to deny")
                                         : qsTr("Enter to allow once, Escape to deny")
            }
        }

        Keys.onReturnPressed: prompt.controller.answerClipboard(1)
        Keys.onEnterPressed: prompt.controller.answerClipboard(1)
        Keys.onEscapePressed: prompt.controller.answerClipboard(0)
        Keys.onPressed: event => {
            if (event.key === Qt.Key_A && prompt.canRemember) {
                prompt.controller.answerClipboard(2)
                event.accepted = true
            }
        }
    }
}
