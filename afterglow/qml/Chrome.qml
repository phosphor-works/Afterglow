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
            width: Math.min(label.implicitWidth + 16 + (paneStatus.visible ? paneStatus.width + 6 : 0), modelData.width * 0.5)
            height: label.implicitHeight + 6
            radius: height / 2
            color: Qt.rgba(root.controller.background.r, root.controller.background.g,
                           root.controller.background.b, 0.85)
            border.color: modelData.focused ? root.controller.accent
                                            : Qt.rgba(root.controller.foreground.r, root.controller.foreground.g,
                                                      root.controller.foreground.b, 0.15)
            StatusIndicator {
                id: paneStatus
                controller: root.controller
                status: parent.modelData.status || ({})
                anchors.left: parent.left
                anchors.leftMargin: 6
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: label
                anchors.left: paneStatus.visible ? paneStatus.right : parent.left
                anchors.leftMargin: paneStatus.visible ? 4 : 8
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 16 - (paneStatus.visible ? paneStatus.width + 2 : 0)
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

    // URL mode: a label to type over each link.
    Repeater {
        model: root.controller.urlHints
        delegate: Rectangle {
            required property var modelData
            x: modelData.x
            y: modelData.y
            z: 50
            width: hintText.implicitWidth + 8
            height: hintText.implicitHeight + 2
            radius: 3
            color: root.controller.warnColor
            Text {
                id: hintText
                anchors.centerIn: parent
                textFormat: Text.StyledText
                // Typed letters dimmed, the rest bold.
                text: "<span style='color:#80000000'>" + parent.modelData.label.substring(0, parent.modelData.typed)
                      + "</span><b>" + parent.modelData.label.substring(parent.modelData.typed) + "</b>"
                color: "#000000"
                font.family: "monospace"
                font.pixelSize: 12
            }
        }
    }

    // Unicode input: the code being typed and what it makes.
    Rectangle {
        visible: root.controller.unicodeInput.active === true
        z: 60
        x: root.controller.unicodeInput.x || 0
        y: (root.controller.unicodeInput.y || 0) + 4
        width: unicodeRow.implicitWidth + 16
        height: unicodeRow.implicitHeight + 10
        radius: 6
        color: Qt.lighter(root.controller.background, 1.2)
        border.color: root.controller.accent
        Row {
            id: unicodeRow
            anchors.centerIn: parent
            spacing: 10
            Text {
                text: "U+" + (root.controller.unicodeInput.text || "")
                color: root.controller.foreground
                font.family: "monospace"
                font.pixelSize: 14
            }
            Text {
                text: root.controller.unicodeInput.preview || "\u2026"
                color: root.controller.foreground
                font.pixelSize: 18
            }
        }
    }

    ClipboardPrompt {
        controller: root.controller
        anchors.fill: root
        visible: root.controller.clipboardPromptOpen
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
