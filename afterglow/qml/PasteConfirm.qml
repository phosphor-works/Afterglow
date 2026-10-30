// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asks before pasting several lines without bracketed paste.
Item {
    id: confirm

    required property QtObject controller

    onVisibleChanged: {
        if (visible)
            panel.forceActiveFocus()
    }

    Rectangle {
        anchors.fill: parent
        color: "#59000000"
        MouseArea {
            anchors.fill: parent
            onClicked: confirm.controller.answerPaste(false)
        }
    }

    Pane {
        id: panel
        anchors.centerIn: parent
        width: Math.min(560, parent.width * 0.85)
        padding: 16

        ColumnLayout {
            width: parent.width
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: qsTr("Paste %1 lines?").arg(confirm.controller.pasteLines)
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("The program is not expecting a paste, so each line runs as it arrives.")
                wrapMode: Text.WordWrap
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(preview.implicitHeight, 160)
                clip: true
                TextArea {
                    id: preview
                    text: confirm.controller.pastePreview
                    textFormat: TextEdit.PlainText
                    font.family: "monospace"
                    readOnly: true
                    selectByMouse: true
                }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button {
                    text: qsTr("Cancel")
                    onClicked: confirm.controller.answerPaste(false)
                }
                Button {
                    text: qsTr("Paste")
                    onClicked: confirm.controller.answerPaste(true)
                }
            }
        }

        Keys.onReturnPressed: confirm.controller.answerPaste(true)
        Keys.onEnterPressed: confirm.controller.answerPaste(true)
        Keys.onEscapePressed: confirm.controller.answerPaste(false)
    }
}
