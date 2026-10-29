// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

Rectangle {
    id: bar

    required property QtObject controller

    color: Qt.darker(controller.background, 1.25)

    readonly property real tabWidth: Math.min(220, Math.max(80, (width - addButton.width) / Math.max(1, controller.tabTitles.length)))

    Row {
        id: tabs
        height: bar.height

        Repeater {
            model: bar.controller.tabTitles

            delegate: Rectangle {
                id: tab

                required property int index
                required property string modelData
                readonly property bool current: index === bar.controller.currentTab

                width: bar.tabWidth
                height: tabs.height
                color: current ? bar.controller.background
                               : (hover.hovered ? Qt.darker(bar.controller.background, 1.1) : "transparent")

                Rectangle {
                    // accent line under the current tab
                    visible: tab.current
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 2
                    color: bar.controller.accent
                }

                StatusIndicator {
                    id: status
                    controller: bar.controller
                    status: bar.controller.tabStatus[tab.index] || ({})
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: visible ? 10 : 0
                    z: 10
                }

                Text {
                    anchors.left: status.right
                    anchors.right: closeButton.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: status.visible ? 6 : 12
                    anchors.rightMargin: 4
                    text: tab.modelData.length > 0 ? tab.modelData : "afterglow"
                    elide: Text.ElideRight
                    color: bar.controller.foreground
                    opacity: tab.current ? 1.0 : 0.6
                    font.pixelSize: 13
                }

                Text {
                    id: closeButton
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: 8
                    text: "×"
                    color: bar.controller.foreground
                    opacity: closeArea.containsMouse ? 1.0 : (hover.hovered ? 0.5 : 0.0)
                    font.pixelSize: 15

                    MouseArea {
                        id: closeArea
                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        onClicked: bar.controller.closeTab(tab.index)
                    }
                }

                HoverHandler {
                    id: hover
                }

                TapHandler {
                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                    onTapped: (point, button) => {
                        if (button === Qt.MiddleButton)
                            bar.controller.closeTab(tab.index)
                        else
                            bar.controller.activateTab(tab.index)
                    }
                }
            }
        }
    }

    Text {
        id: addButton
        anchors.left: tabs.right
        anchors.verticalCenter: parent.verticalCenter
        width: 32
        horizontalAlignment: Text.AlignHCenter
        text: "+"
        color: bar.controller.foreground
        opacity: addArea.containsMouse ? 1.0 : 0.6
        font.pixelSize: 16

        MouseArea {
            id: addArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: bar.controller.newTab()
        }
    }
}
