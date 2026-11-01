// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asks to go ahead with something, such as closing a tab with running jobs.
// Return accepts only while the panel itself has the focus, so Return on a
// focused Cancel button cancels.
Item {
    id: prompt

    required property QtObject controller
    readonly property var spec: controller.confirmPrompt

    function syncVisibility() {
        if (visible)
            Qt.callLater(() => { if (prompt.visible) takeFocus() })
    }

    function takeFocus() {
        panel.forceActiveFocus()
    }

    onVisibleChanged: syncVisibility()
    Component.onCompleted: Qt.callLater(syncVisibility)

    Rectangle {
        anchors.fill: parent
        color: prompt.controller.scrimColor
        MouseArea {
            anchors.fill: parent
            onClicked: prompt.controller.answerConfirm(false)
        }
    }

    Pane {
        id: panel
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.2
        width: Math.min(460, parent.width * 0.85)
        padding: 16

        ColumnLayout {
            width: parent.width
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: prompt.spec.heading || ""
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: prompt.spec.body || ""
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button {
                    text: qsTr("Cancel")
                    onClicked: prompt.controller.answerConfirm(false)
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                }
                Button {
                    text: prompt.spec.accept || qsTr("OK")
                    onClicked: prompt.controller.answerConfirm(true)
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                }
            }
        }

        Keys.onReturnPressed: if (panel.Window.activeFocusItem === panel) prompt.controller.answerConfirm(true)
        Keys.onEnterPressed: if (panel.Window.activeFocusItem === panel) prompt.controller.answerConfirm(true)
        Keys.onEscapePressed: prompt.controller.answerConfirm(false)
    }
}
