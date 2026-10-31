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
            Qt.callLater(() => { if (prompt.visible) input.forceActiveFocus() })
        }
    }

    onVisibleChanged: syncVisibility()
    Component.onCompleted: Qt.callLater(syncVisibility)

    Rectangle {
        anchors.fill: parent
        color: "#59000000"
        MouseArea {
            anchors.fill: parent
            onClicked: prompt.controller.cancelTitle()
        }
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
                wrapMode: Text.WordWrap
                font.bold: true
            }
            TextField {
                id: input
                Layout.fillWidth: true
                placeholderText: qsTr("Empty follows the program's title")
                Accessible.name: prompt.controller.titlePrompt.heading || qsTr("Title")
                selectByMouse: true
                maximumLength: 256
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
