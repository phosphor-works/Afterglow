// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asks before a program reads or replaces the clipboard (OSC 52 or OSC
// 5522). A program can open this prompt while the user types, so nothing
// here allows on a key typed for the shell: Deny holds the focus, Return
// answers only the button that has it, and "a" counts only when pressed on
// its own.
Item {
    id: prompt

    required property QtObject controller
    readonly property bool canRemember: controller.clipboardPromptName.length > 0

    // Called by Chrome.qml when this is the topmost open overlay.
    function takeFocus() {
        deny.forceActiveFocus()
    }

    Scrim {
        controller: prompt.controller
        onDismissed: prompt.controller.answerClipboard(0)
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
                    onClicked: prompt.controller.answerClipboard(1)
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                }
                // The text above already names the program. A plain label keeps
                // a name from the terminal out of the mnemonic and rich text steps.
                Button {
                    visible: prompt.canRemember
                    text: qsTr("Always allow")
                    onClicked: prompt.controller.answerClipboard(2)
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                }
                Button {
                    id: deny
                    text: qsTr("Deny")
                    highlighted: true
                    onClicked: prompt.controller.answerClipboard(0)
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: prompt.canRemember ? qsTr("Tab chooses a button and Enter presses it. Press a to always allow. Escape denies.")
                                         : qsTr("Tab chooses a button and Enter presses it. Escape denies.")
            }
        }

        Keys.onEscapePressed: prompt.controller.answerClipboard(0)
        Keys.onPressed: event => {
            if (event.key === Qt.Key_A && prompt.canRemember && !event.isAutoRepeat
                && (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier) {
                prompt.controller.answerClipboard(2)
                event.accepted = true
            }
        }
    }
}
