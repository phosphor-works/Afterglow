// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asks before a paste that holds a line end when the program has no
// bracketed paste, because the shell then runs each line as it arrives.
// Return pastes only while the panel itself has the focus: a focused button
// answers for itself, and Return in the preview does nothing.
Item {
    id: confirm

    required property QtObject controller

    function syncVisibility() {
        if (visible)
            Qt.callLater(() => { if (confirm.visible) takeFocus() })
    }

    function takeFocus() {
        panel.forceActiveFocus()
    }

    onVisibleChanged: syncVisibility()
    Component.onCompleted: Qt.callLater(syncVisibility)

    Rectangle {
        anchors.fill: parent
        color: confirm.controller.scrimColor
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
                text: confirm.controller.pasteLines === 1 ? qsTr("Paste 1 line?")
                                                          : qsTr("Paste %1 lines?").arg(confirm.controller.pasteLines)
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
            Label {
                Layout.fillWidth: true
                visible: confirm.controller.pasteHiddenBytes > 0
                text: confirm.controller.pasteHiddenBytes === 1 ? qsTr("The preview leaves out 1 more byte.")
                      : qsTr("The preview leaves out %1 more bytes.").arg(confirm.controller.pasteHiddenBytes)
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button {
                    text: qsTr("Cancel")
                    onClicked: confirm.controller.answerPaste(false)
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                }
                Button {
                    text: qsTr("Paste")
                    onClicked: confirm.controller.answerPaste(true)
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                }
            }
        }

        Keys.onReturnPressed: if (panel.Window.activeFocusItem === panel) confirm.controller.answerPaste(true)
        Keys.onEnterPressed: if (panel.Window.activeFocusItem === panel) confirm.controller.answerPaste(true)
        Keys.onEscapePressed: confirm.controller.answerPaste(false)
    }
}
