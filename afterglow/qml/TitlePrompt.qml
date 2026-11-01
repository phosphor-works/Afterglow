// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: prompt

    required property QtObject controller

    function syncVisibility() {
        if (visible) {
            input.text = controller.titlePrompt.text || ""
            input.selectAll()
        }
    }

    function takeFocus() {
        input.forceActiveFocus()
    }

    onVisibleChanged: syncVisibility()
    Component.onCompleted: Qt.callLater(syncVisibility)

    // A second request while the prompt is open (a script renames another
    // tab) changes the target: show that target's title, not the old text.
    Connections {
        target: prompt.controller
        function onTitlePromptChanged() {
            if (prompt.visible && prompt.controller.titlePrompt.open === true)
                prompt.syncVisibility()
        }
    }

    Scrim {
        controller: prompt.controller
        onDismissed: prompt.controller.cancelTitle()
    }

    Pane {
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.2
        width: Math.min(460, parent.width * 0.85)
        padding: 16

        ColumnLayout {
            width: parent.width
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: prompt.controller.titlePrompt.heading || ""
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                font.bold: true
            }
            TextField {
                id: input
                Layout.fillWidth: true
                placeholderText: qsTr("Empty follows the program's title")
                Accessible.name: prompt.controller.titlePrompt.heading || qsTr("Title")
                selectByMouse: true
                // Never cut the current title, which a script may have set longer.
                maximumLength: Math.max(256, (prompt.controller.titlePrompt.text || "").length)
                onAccepted: prompt.controller.answerTitle(text)
                Keys.onEscapePressed: prompt.controller.cancelTitle()
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button {
                    text: qsTr("Cancel")
                    onClicked: prompt.controller.cancelTitle()
                }
                Button {
                    text: qsTr("Save")
                    highlighted: true
                    onClicked: prompt.controller.answerTitle(input.text)
                }
            }
        }
        Keys.onEscapePressed: prompt.controller.cancelTitle()
    }
}
