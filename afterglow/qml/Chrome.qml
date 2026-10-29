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
        if (commandPalette.visible)
            return true
        if (searchBar.visible) {
            const p = searchBar.mapFromItem(root, x, y)
            return searchBar.contains(p)
        }
        return false
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

    CommandPalette {
        id: commandPalette
        controller: root.controller
        anchors.fill: root
        visible: root.controller.paletteOpen
    }
}
