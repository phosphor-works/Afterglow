// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: commandPalette

    required property QtObject controller

    property string filter: ""
    readonly property int rowHeight: Math.max(36, Math.ceil(metrics.height) + 16)
    FontMetrics { id: metrics; font: input.font }
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
            Qt.callLater(() => { if (commandPalette.visible) input.forceActiveFocus() })
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
        anchors.centerIn: parent
        width: Math.max(0, Math.min(620, parent.width - 32))
        height: Math.max(0, Math.min(content.implicitHeight + topPadding + bottomPadding, parent.height - 32))
        padding: 12
        clip: true

        ColumnLayout {
            id: content
            anchors.fill: parent
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: qsTr("Commands")
                font.bold: true
                elide: Text.ElideRight
            }

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
                Layout.minimumHeight: 0
                Layout.preferredHeight: Math.max(1, Math.min(8, count)) * commandPalette.rowHeight
                clip: true
                model: commandPalette.matches
                highlightMoveDuration: 0
                boundsBehavior: Flickable.StopAtBounds
                onCurrentIndexChanged: {
                    if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
                }
                ScrollBar.vertical: ScrollBar {
                    id: scrollBar
                    policy: ScrollBar.AsNeeded
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("No matching commands")
                    visible: list.count === 0
                    elide: Text.ElideRight
                }

                delegate: ItemDelegate {
                    id: row
                    required property int index
                    required property var modelData

                    width: Math.max(0, list.width - (scrollBar.visible ? scrollBar.width + 8 : 0))
                    height: commandPalette.rowHeight
                    text: modelData.title
                    highlighted: ListView.isCurrentItem
                    focusPolicy: Qt.NoFocus
                    rightPadding: shortcut.width + 24
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
                        width: Math.min(implicitWidth, row.width * 0.4)
                        text: row.modelData.shortcut
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: row.highlighted ? row.palette.highlightedText : row.palette.text
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: list.count === 1 ? qsTr("1 command") : qsTr("%1 commands").arg(list.count)
                    elide: Text.ElideRight
                }
                Label {
                    visible: panel.width >= 420
                    text: qsTr("↑↓ Navigate · Enter Run · Esc Close")
                }
            }
        }
    }
}
