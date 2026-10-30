// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Keep the existing Enter/Escape answers while using desktop controls.
Item {
    id: prompt

    required property QtObject controller
    readonly property var spec: controller.confirmPrompt

    onVisibleChanged: {
        if (visible)
            panel.forceActiveFocus()
    }

    Rectangle {
        anchors.fill: parent
        color: "#59000000"
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
                }
                Button {
                    text: prompt.spec.accept || qsTr("OK")
                    onClicked: prompt.controller.answerConfirm(true)
                }
            }
        }

        Keys.onReturnPressed: prompt.controller.answerConfirm(true)
        Keys.onEnterPressed: prompt.controller.answerConfirm(true)
        Keys.onEscapePressed: prompt.controller.answerConfirm(false)
    }
}
