// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

Item {
    id: commandPalette

    required property QtObject controller

    property string filter: ""
    readonly property var matches: {
        const words = filter.toLowerCase().split(/\s+/).filter(w => w.length > 0)
        return controller.actions.filter(a => {
            const t = a.title.toLowerCase()
            return words.every(w => t.includes(w))
        })
    }

    onVisibleChanged: {
        if (visible) {
            input.text = ""
            list.currentIndex = 0
            input.forceActiveFocus()
        }
    }

    function run(index: int) {
        if (index >= 0 && index < matches.length)
            controller.runAction(matches[index].id)
    }

    // Dim the terminal; clicking outside the panel closes the palette.
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.35
        MouseArea {
            anchors.fill: parent
            onClicked: commandPalette.controller.paletteOpen = false
        }
    }

    Rectangle {
        id: panel
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.15
        width: Math.min(620, parent.width * 0.85)
        height: Math.min(header.height + list.contentHeight + 12, parent.height * 0.7)
        radius: 8
        color: Qt.lighter(commandPalette.controller.background, 1.15)
        border.color: Qt.rgba(commandPalette.controller.foreground.r, commandPalette.controller.foreground.g,
                              commandPalette.controller.foreground.b, 0.15)
        clip: true

        Item {
            id: header
            width: parent.width
            height: 44

            TextInput {
                id: input
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                verticalAlignment: TextInput.AlignVCenter
                color: commandPalette.controller.foreground
                selectionColor: commandPalette.controller.accent
                font.pixelSize: 15
                onTextChanged: {
                    commandPalette.filter = text
                    list.currentIndex = 0
                }
                Keys.onEscapePressed: commandPalette.controller.paletteOpen = false
                Keys.onUpPressed: list.currentIndex = Math.max(0, list.currentIndex - 1)
                Keys.onDownPressed: list.currentIndex = Math.min(list.count - 1, list.currentIndex + 1)
                Keys.onReturnPressed: commandPalette.run(list.currentIndex)
                Keys.onEnterPressed: commandPalette.run(list.currentIndex)

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: input.text.length === 0
                    text: "Type a command"
                    color: commandPalette.controller.foreground
                    opacity: 0.4
                    font: input.font
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Qt.rgba(commandPalette.controller.foreground.r, commandPalette.controller.foreground.g,
                               commandPalette.controller.foreground.b, 0.1)
            }
        }

        ListView {
            id: list
            anchors.top: header.bottom
            anchors.topMargin: 6
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 6
            clip: true
            model: commandPalette.matches
            highlightMoveDuration: 0

            delegate: Rectangle {
                id: row

                required property int index
                required property var modelData

                width: list.width
                height: 32
                radius: 5
                color: ListView.isCurrentItem ? Qt.rgba(commandPalette.controller.accent.r, commandPalette.controller.accent.g,
                                                        commandPalette.controller.accent.b, 0.25) : "transparent"

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.title
                    color: commandPalette.controller.foreground
                    font.pixelSize: 14
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.shortcut
                    color: commandPalette.controller.foreground
                    opacity: 0.5
                    font.pixelSize: 12
                }
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: list.currentIndex = row.index
                    onClicked: commandPalette.run(row.index)
                }
            }
        }
    }
}
