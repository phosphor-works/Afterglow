// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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

    Rectangle {
        anchors.fill: parent
        color: "#59000000"
        MouseArea {
            anchors.fill: parent
            onClicked: commandPalette.controller.paletteOpen = false
        }
    }

    Pane {
        id: panel
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.15
        width: Math.min(620, parent.width * 0.85)
        height: Math.min(input.implicitHeight + list.contentHeight + 8 + topPadding + bottomPadding, parent.height * 0.7)
        padding: 8

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            TextField {
                id: input
                Layout.fillWidth: true
                placeholderText: qsTr("Type a command")
                Accessible.name: placeholderText
                selectByMouse: true
                onTextChanged: {
                    commandPalette.filter = text
                    list.currentIndex = 0
                }
                Keys.onEscapePressed: commandPalette.controller.paletteOpen = false
                Keys.onUpPressed: list.currentIndex = Math.max(0, list.currentIndex - 1)
                Keys.onDownPressed: list.currentIndex = Math.min(list.count - 1, list.currentIndex + 1)
                Keys.onReturnPressed: commandPalette.run(list.currentIndex)
                Keys.onEnterPressed: commandPalette.run(list.currentIndex)
            }

            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: commandPalette.matches
                highlightMoveDuration: 0
                ScrollBar.vertical: ScrollBar {}

                delegate: ItemDelegate {
                    id: row
                    required property int index
                    required property var modelData

                    width: list.width
                    text: modelData.title
                    highlighted: ListView.isCurrentItem
                    focusPolicy: Qt.NoFocus
                    rightPadding: shortcut.implicitWidth + 24
                    rightInset: leftInset
                    onHoveredChanged: {
                        if (hovered)
                            list.currentIndex = index
                    }
                    onClicked: commandPalette.run(index)
                    Accessible.description: modelData.shortcut

                    Label {
                        id: shortcut
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.shortcut
                        color: row.highlighted ? row.palette.highlightedText : row.palette.text
                    }
                }
            }
        }
    }
}
