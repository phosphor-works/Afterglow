// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// Asks before pasting several lines into a program without bracketed paste,
// where each line would run as soon as it arrives.
Item {
    id: confirm

    required property QtObject controller

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
            onClicked: confirm.controller.answerPaste(false)
        }
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(560, parent.width * 0.85)
        height: column.implicitHeight + 32
        radius: 8
        color: Qt.lighter(confirm.controller.background, 1.15)
        border.color: Qt.rgba(confirm.controller.foreground.r, confirm.controller.foreground.g,
                              confirm.controller.foreground.b, 0.15)

        Column {
            id: column
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            Text {
                width: parent.width
                text: "Paste " + confirm.controller.pasteLines + " lines?"
                color: confirm.controller.foreground
                font.pixelSize: 15
                font.bold: true
            }
            Text {
                width: parent.width
                text: "The program is not expecting a paste, so each line runs as it arrives."
                wrapMode: Text.WordWrap
                color: confirm.controller.foreground
                opacity: 0.7
                font.pixelSize: 13
            }
            Rectangle {
                width: parent.width
                height: Math.min(preview.implicitHeight + 12, 160)
                radius: 4
                color: confirm.controller.background
                clip: true
                Text {
                    id: preview
                    x: 6
                    y: 6
                    width: parent.width - 12
                    text: confirm.controller.pastePreview
                    color: confirm.controller.foreground
                    font.family: "monospace"
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    maximumLineCount: 8
                }
            }
            Text {
                text: "Enter to paste, Escape to cancel"
                color: confirm.controller.foreground
                opacity: 0.5
                font.pixelSize: 12
            }
        }
    }

    Item {
        id: keys
        focus: true
        Keys.onReturnPressed: confirm.controller.answerPaste(true)
        Keys.onEnterPressed: confirm.controller.answerPaste(true)
        Keys.onEscapePressed: confirm.controller.answerPaste(false)
    }
}
