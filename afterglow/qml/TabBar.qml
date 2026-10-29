// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// The tab bar. Tabs share the width between a minimum and a maximum; past
// that the bar scrolls (mouse wheel) and keeps the current tab in view.
// Press a tab to switch to it and drag it sideways to move it; middle-click
// closes, double-click renames, right-click opens its menu. Double-clicking
// the empty bar opens a new tab.
Rectangle {
    id: bar

    required property QtObject controller

    readonly property color ink: controller.foreground
    readonly property int count: controller.tabTitles.length
    readonly property real minTab: 110
    readonly property real maxTab: 220
    readonly property real available: width - addButton.width - 4
    readonly property real tabWidth: Math.max(minTab, Math.min(maxTab, available / Math.max(1, count)))

    // Dragging: the tab being moved, where it would land, and where it is drawn.
    property int dragFrom: -1
    property int dragTo: -1
    property real dragX: 0
    // Set for the frame a move lands in, so tabs take their new places at once.
    property bool settling: false
    // Double-clicks, from two presses: the window passes both on as presses.
    property double lastPress: 0
    property int lastPressTab: -2

    // True when this press, on tab @p index (-1 for the empty bar), is the
    // second of a double-click.
    function secondClick(index: int): bool {
        const now = Date.now()
        const second = lastPressTab === index && now - lastPress < Qt.styleHints.mouseDoubleClickInterval
        lastPress = second ? 0 : now
        lastPressTab = index
        return second
    }

    color: Qt.darker(controller.background, 1.3)

    // Where tab @p i is drawn while another is being dragged.
    function slotOf(i: int): int {
        if (dragFrom < 0 || i === dragFrom)
            return i
        if (dragFrom < dragTo && i > dragFrom && i <= dragTo)
            return i - 1
        if (dragTo < dragFrom && i >= dragTo && i < dragFrom)
            return i + 1
        return i
    }

    function revealCurrent() {
        const left = controller.currentTab * tabWidth
        if (left < strip.contentX)
            strip.contentX = left
        else if (left + tabWidth > strip.contentX + strip.width)
            strip.contentX = left + tabWidth - strip.width
    }

    Connections {
        target: bar.controller
        function onTabsChanged() { Qt.callLater(bar.revealCurrent) }
    }
    onWidthChanged: Qt.callLater(revealCurrent)

    // The empty bar: double-click for a new tab.
    MouseArea {
        anchors.fill: parent
        onPressed: {
            if (bar.secondClick(-1))
                bar.controller.newTab()
        }
    }

    Flickable {
        id: strip
        x: 0
        width: Math.min(bar.available, contentWidth)
        height: bar.height
        contentWidth: bar.count * bar.tabWidth
        contentHeight: height
        interactive: false
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Behavior on contentX {
            enabled: bar.dragFrom < 0
            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
        }

        WheelHandler {
            onWheel: event => {
                const delta = event.angleDelta.y !== 0 ? event.angleDelta.y : event.angleDelta.x
                const max = Math.max(0, strip.contentWidth - strip.width)
                strip.contentX = Math.max(0, Math.min(max, strip.contentX - delta))
            }
        }

        Repeater {
            // By count, not titles: tabs keep their items (and hover, clicks
            // under way and animations) when titles or the current tab change.
            model: bar.count

            delegate: Item {
                id: tab

                required property int index
                readonly property string title: bar.controller.tabTitles[index] || ""
                readonly property bool current: index === bar.controller.currentTab
                readonly property bool dragged: index === bar.dragFrom
                readonly property bool hovered: body.containsMouse || closeArea.containsMouse

                width: bar.tabWidth
                height: strip.height
                z: dragged ? 10 : (current ? 2 : 1)
                x: dragged ? bar.dragX : bar.slotOf(index) * bar.tabWidth

                Behavior on x {
                    enabled: !tab.dragged && !bar.settling
                    NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
                }

                Rectangle {
                    anchors.fill: parent
                    anchors.topMargin: 3
                    radius: 6
                    // Only the top corners round: the bottom joins the terminal.
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.radius
                        color: parent.color
                    }
                    color: tab.current || tab.dragged ? bar.controller.background
                         : tab.hovered ? Qt.rgba(bar.ink.r, bar.ink.g, bar.ink.b, 0.07) : "transparent"
                    opacity: tab.dragged ? 0.95 : 1.0

                    Rectangle {
                        visible: tab.current
                        anchors.top: parent.top
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: parent.width - 2 * parent.radius
                        height: 2
                        radius: 1
                        color: bar.controller.accent
                    }
                }

                // A separator between two tabs that are neither current nor hovered.
                Rectangle {
                    readonly property int next: tab.index + 1
                    visible: !tab.current && !tab.hovered && !tab.dragged && next < bar.count
                             && next !== bar.controller.currentTab && bar.dragFrom < 0
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: 1
                    width: 1
                    height: parent.height * 0.45
                    color: Qt.rgba(bar.ink.r, bar.ink.g, bar.ink.b, 0.15)
                }

                StatusIndicator {
                    id: status
                    controller: bar.controller
                    status: bar.controller.tabStatus[tab.index] || ({})
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: 1
                    anchors.leftMargin: visible ? 12 : 0
                    z: 10
                }

                Text {
                    anchors.left: status.right
                    anchors.right: closeButton.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: 1
                    anchors.leftMargin: status.visible ? 6 : 14
                    anchors.rightMargin: 4
                    text: tab.title.length > 0 ? tab.title : "afterglow"
                    elide: Text.ElideRight
                    color: bar.ink
                    opacity: tab.current ? 1.0 : (tab.hovered ? 0.8 : 0.55)
                    font.pixelSize: 13
                }

                // The tab itself: switch, drag to move, and the other buttons.
                MouseArea {
                    id: body
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: bar.dragFrom >= 0 ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                    property real pressX: 0
                    property real grabOffset: 0

                    onPressed: mouse => {
                        if (mouse.button === Qt.LeftButton) {
                            pressX = mapToItem(strip.contentItem, mouse.x, 0).x
                            grabOffset = mouse.x
                            if (bar.secondClick(tab.index))
                                bar.controller.renameTab(tab.index)
                            else if (!tab.current)
                                bar.controller.activateTab(tab.index)
                        }
                    }
                    onPositionChanged: mouse => {
                        if (!(mouse.buttons & Qt.LeftButton) || bar.count < 2)
                            return
                        const at = mapToItem(strip.contentItem, mouse.x, 0).x
                        if (bar.dragFrom < 0 && Math.abs(at - pressX) < 6)
                            return
                        const max = (bar.count - 1) * bar.tabWidth
                        bar.dragX = Math.max(0, Math.min(max, at - grabOffset))
                        bar.dragTo = Math.max(0, Math.min(bar.count - 1, Math.round(bar.dragX / bar.tabWidth)))
                        bar.dragFrom = tab.index
                    }
                    onReleased: mouse => {
                        if (mouse.button === Qt.LeftButton && bar.dragFrom >= 0) {
                            const from = bar.dragFrom
                            const to = bar.dragTo
                            bar.settling = true
                            bar.dragFrom = -1
                            bar.dragTo = -1
                            if (to !== from)
                                bar.controller.moveTab(from, to)
                            Qt.callLater(() => bar.settling = false)
                        }
                    }
                    onClicked: mouse => {
                        if (mouse.button === Qt.MiddleButton)
                            bar.controller.closeTab(tab.index)
                        else if (mouse.button === Qt.RightButton) {
                            const p = mapToItem(bar, mouse.x, mouse.y)
                            bar.controller.tabMenu(tab.index, p.x, p.y)
                        }
                    }
                }

                // Above the tab's own area, so a click here only closes.
                Rectangle {
                    id: closeButton
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: 1
                    anchors.rightMargin: 8
                    width: 20
                    height: 20
                    radius: 4
                    z: 20
                    visible: (tab.current || tab.hovered) && !tab.dragged
                    color: closeArea.containsMouse ? Qt.rgba(bar.ink.r, bar.ink.g, bar.ink.b, 0.15) : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "×"
                        color: bar.ink
                        opacity: closeArea.containsMouse ? 1.0 : 0.55
                        font.pixelSize: 16
                    }

                    MouseArea {
                        id: closeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                        onClicked: bar.controller.closeTab(tab.index)
                    }
                }
            }
        }
    }

    Rectangle {
        id: addButton
        x: strip.x + strip.width + 4
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: 1
        width: 26
        height: 22
        radius: 5
        color: addArea.containsMouse ? Qt.rgba(bar.ink.r, bar.ink.g, bar.ink.b, 0.12) : "transparent"

        Text {
            anchors.centerIn: parent
            text: "+"
            color: bar.ink
            opacity: addArea.containsMouse ? 1.0 : 0.6
            font.pixelSize: 17
        }

        MouseArea {
            id: addArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: bar.controller.newTab()
        }
    }
}
