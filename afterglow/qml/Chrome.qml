// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// The window chrome, drawn over the terminal into a texture on afterglow's
// GPU device. Transparent everywhere except the tab bar and open overlays.
Item {
    id: root

    required property QtObject controller

    // Called by the host for every pointer event: true if the chrome takes it.
    function wantsPointer(x: real, y: real): bool {
        if (tabBar.visible && y < tabBar.height)
            return true
        if (commandPalette.visible || pasteConfirm.visible)
            return true
        if (searchBar.visible) {
            const p = searchBar.mapFromItem(root, x, y)
            return searchBar.contains(p)
        }
        return false
    }

    // Split labels: a small title in each pane's top-right corner.
    Repeater {
        model: root.controller.paneTitles
        delegate: Rectangle {
            required property var modelData
            x: modelData.x + modelData.width - width - 8
            y: modelData.y + 6
            width: Math.min(label.implicitWidth + 16, modelData.width * 0.5)
            height: label.implicitHeight + 6
            radius: height / 2
            color: Qt.rgba(root.controller.background.r, root.controller.background.g,
                           root.controller.background.b, 0.85)
            border.color: modelData.focused ? root.controller.accent
                                            : Qt.rgba(root.controller.foreground.r, root.controller.foreground.g,
                                                      root.controller.foreground.b, 0.15)
            Text {
                id: label
                anchors.centerIn: parent
                width: parent.width - 16
                horizontalAlignment: Text.AlignHCenter
                text: parent.modelData.title.length > 0 ? parent.modelData.title : "shell"
                elide: Text.ElideMiddle
                color: root.controller.foreground
                opacity: parent.modelData.focused ? 0.95 : 0.6
                font.pixelSize: 11
            }
        }
    }

    TabBar {
        id: tabBar
        controller: root.controller
        width: root.width
        height: root.controller.tabBarHeight > 0 ? root.controller.tabBarHeight : 0
        visible: root.controller.tabBarHeight > 0
    }

    SearchBar {
        id: searchBar
        controller: root.controller
        anchors.top: tabBar.visible ? tabBar.bottom : root.top
        anchors.right: root.right
        anchors.margins: 8
        visible: root.controller.searchOpen
    }

    PasteConfirm {
        id: pasteConfirm
        controller: root.controller
        anchors.fill: root
        visible: root.controller.pasteConfirmOpen
    }

    CommandPalette {
        id: commandPalette
        controller: root.controller
        anchors.fill: root
        visible: root.controller.paletteOpen
    }
}
