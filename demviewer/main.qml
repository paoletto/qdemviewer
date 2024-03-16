/****************************************************************************
**
** Copyright (C) 2024- Paolo Angelelli <paoletto@gmail.com>
**
** Commercial License Usage
** Licensees holding a valid commercial qdemviewer license may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement with the copyright holder. For licensing terms
** and conditions and further information contact the copyright holder.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3. The licenses are as published by
** the Free Software Foundation at https://www.gnu.org/licenses/gpl-3.0.html,
** with the exception that the use of this work for training artificial intelligence
** is prohibited for both commercial and non-commercial use.
**
****************************************************************************/

import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Controls 2.15 as QC2
import QtQuick.Layouts 1.15
import QtQuick.Controls 1.4 as QC1
import QtPositioning 5.15
import QtLocation 5.15
import Qt.labs.settings 1.1
import DemViewer 1.0

QC2.ApplicationWindow {
    id: root
    width: 1400
    height: 700
    visible: true
    title: qsTr("DEM Viewer")

    Shortcut {
        sequence: StandardKey.Quit
        onActivated: {
            settings.modelTransformation = arcball.modelTransformation
            Qt.quit()
        }
    }

    property var urlTemplates: []
    function pushTemplate(name_ = "", template_ = "") {
        var record = {name : name_, template : template_}
        if (root.urlTemplates.length !== 0
                && root.urlTemplates[root.urlTemplates.length - 1].name == ""
                && root.urlTemplates[root.urlTemplates.length - 1].template == "")
            return;
        var newTemplates = root.urlTemplates
        newTemplates.push(record)
        root.urlTemplates = newTemplates
    }
    function removeTemplate(idx) {
        var newTemplates = []
        for (var i = 0; i < root.urlTemplates.length; i++) {
            if (i !== idx)
                newTemplates.push(root.urlTemplates[i])
        }
        root.urlTemplates = newTemplates
        if (newTemplates.length == 0)
            pushTemplate()
    }

    Settings {
        id: settings
        property alias urlTemplates: root.urlTemplates
        property alias mapCenter: overlay.center
        property alias mapZoomLevel: overlay.zoomLevel
        property alias appx: root.x
        property alias appy: root.y
        property alias elevationSliderValue: elevationSlider.value
        property alias zlsliderValue: zlslider.value
        property alias zlMapSliderValue: zlMapSlider.value
        property alias brightness: brightnessSlider.value
        property alias rasterEnabled: rasterEnabled.checked
        property alias offline: offline.checked
        property alias invertTessDirection: invertTessDirection.checked
        property alias lightPos: shadingSphere.pos
        property alias joinTiles: joinTilesMenuItem.checked
        property var modelTransformation
    }

    menuBar: QC2.MenuBar {
        QC2.Menu {
            title: qsTr("Map")
            QC2.ActionGroup {
                id: provisioningActions
                exclusive: true
            }
            QC2.Action {
                id: tileMode
                checkable: true;
                checked: true;
                text: qsTr("Tile mode")
                QC2.ActionGroup.group: provisioningActions
            }
            QC2.Action {
                id: coverageMode
                checkable: true;
                text: qsTr("Coverage mode")
                QC2.ActionGroup.group: provisioningActions
            }
            QC2.Action {
                id: clipInCoverage
                checkable: true;
                text: qsTr("Clip in coverage mode")
            }
            QC2.Action {
                id: rasterEnabled
                checkable: true;
                text: qsTr("Map texture")
                enabled: mapFetcher.urlTemplate !== ""
            }
            QC2.Action {
                id: offline
                text: qsTr("Offline")
                checkable: true
                checked: false
            }
            QC2.Action {
                text: qsTr("Clear Data")
                onTriggered: viewer.reset()
            }
        }
        QC2.Menu {
            title: qsTr("Provider")
            Repeater {
                model: root.urlTemplates
                id: repeaterProviders
                delegate: QC2.MenuItem {
                    checkable: true;
                    text: modelData.name
                    property string template: modelData.template
                    QC2.ToolTip {
                        visible: hovered
                        delay: 300
                        text: template
                    }
                    onCheckedChanged: {
                        if (checked) {
                            console.log("Selecting ", template)
                            mapFetcher.urlTemplate = template

                            for (let i = 0; i < repeaterProviders.count; i++) {
                                if (i != index) {
                                    repeaterProviders.itemAt(i).checked = false
                                }
                            }
                        }
                    }

                    QC2.Button {
                        anchors.right: parent.right
                        anchors.rightMargin: 2
                        anchors.verticalCenter: parent.verticalCenter
                        icon.name: "edit-delete"
                        icon.height: height
                        height: parent.height * 1.2
                        width: height
                        onClicked: {
                            console.log("Delete ", index)
                            root.removeTemplate(index)
                        }
                    }
                }
            }
            QC2.Action {
                id: addProvider
                checkable: false;
                text: qsTr("Add Provider")
                onTriggered: {
                    addProviderDialog.visible = true
                }
            }
        }
        QC2.Menu {
            title: qsTr("Rendering")
            QC2.MenuItem {
                id: joinTilesMenuItem
                text: qsTr("Join Tiles")
                checkable: true
            }
            QC2.Action {
                id: invertTessDirection
                checkable: true;
                text: qsTr("Alt tessellation")
            }
            RowLayout {
                spacing: 4
                width: parent.width
                Text {
                    text: "Bri"
                    color: "white"
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: 2
                }

                QC2.Slider {
                    id: brightnessSlider
                    from: 0.5
                    to: 2.0
                    value: 1
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter

                    QC2.ToolTip {
                        parent: brightnessSlider.handle
                        visible: brightnessSlider.pressed
                        text: brightnessSlider.value.toFixed(2)
                    }
                }
            }
            QC2.Action {
                text: qsTr("Reset Scene")
                onTriggered: arcball.reset()
            }
            ColumnLayout {
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Light direction"
                    color: "white"
                }

                Rectangle {
                    id: shadingSphere
                    Layout.alignment: Qt.AlignHCenter
                    Layout.leftMargin: 4
                    Layout.preferredWidth: brightnessSlider.width
                    Layout.preferredHeight: width
                    Layout.topMargin: 4
                    radius: width * .5
                    color: "grey"
                    border.color: "black"
                    border.width: 1
                    smooth: true
                    Rectangle {
                        anchors.centerIn: parent
                        width: 4
                        height: width
                        smooth: true
                        radius: width * .5
                        color: "black"
                        x: parent.width * .5
                        y: parent.height * .5
                        anchors.alignWhenCentered: false
                        antialiasing: true
                    }
                    property point pos: Qt.point(-0.2, -0.2)


                    Rectangle {
                        visible: parent.pos !== undefined
                        width: 3
                        height: width
                        smooth: true
                        radius: width * .5
                        color: "red"
                        x: (parent.pos !== undefined) ? (parent.pos.x + 1.) * parent.width * .5 : null
                        y: (parent.pos !== undefined) ? (parent.pos.y + 1.) * parent.height * .5 : null
                        anchors.alignWhenCentered: false
                        antialiasing: true
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: updateLight(mouse)
                        onPositionChanged: updateLight(mouse)
                        function updateLight(mouse) {
                            var pt = (Qt.point((mouse.x - (width * .5)) / (width * .5),
                                               (mouse.y - (height * .5)) / (height * .5)))
                            parent.pos = arcball.normalizeIfNeeded(pt)
                        }
                    }

                }
            }
        }

    }

    onWidthChanged: arcball.setSize(Qt.size(width, height))
    onHeightChanged: arcball.setSize(Qt.size(width, height))
    Component.onCompleted: {
        if (root.urlTemplates.length == 0) {
            root.pushTemplate("osm", "https://tile.openstreetmap.org/{z}/{x}/{y}.png")
        }
        arcball.setSize(Qt.size(width, height))
    }

    QC2.SplitView {
        anchors.fill: parent
        Item {
            QC2.SplitView.preferredWidth: parent.width * .5
//            title: "Map"
            Item {
                anchors.fill: parent
                Map {
                    id: basemap
                    center: overlay.center
                    zoomLevel: overlay.zoomLevel
                    anchors.fill: parent
                    gesture.enabled: false
                    opacity: 0.99
                    plugin: Plugin {
                        name: "osm"
                        PluginParameter {
                            name: "osm.mapping.providersrepository.disabled"
                            value: true
                        }
                        PluginParameter {
                            name: "osm.mapping.cache.disk.cost_strategy"
                            value: "unitary"
                        }
                        PluginParameter {
                            name: "osm.mapping.cache.disk.size"
                            value: 1000
                        }
                    }
                    Text {
                        anchors {
                            top: parent.top
                            topMargin: 10
                            left: parent.left
                            leftMargin: 10
                        }
                        text: "zoom: " + parent.zoomLevel.toFixed(1)
                    }
                }
                Map {
                    id: overlay
                    anchors.fill: basemap
                    color: "transparent"
                    opacity: 0.9

                    // Do not allow rotation, to avoid headaches
                    gesture.acceptedGestures: MapGestureArea.PanGesture | MapGestureArea.FlickGesture | MapGestureArea.PinchGesture
                    center: QtPositioning.coordinate(60.39, 5.32)
                    zoomLevel: 9

                    plugin: Plugin {
                        name: "itemsoverlay"
                    }

                    property var selectionTL: undefined
                    property var selectionBR: undefined

                    MapRectangle {
                        topLeft: parent.selectionTL
                        bottomRight: parent.selectionBR
                        visible: topLeft !== undefined && bottomRight !== undefined
                        color: "transparent"
                        border.color: "red"
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        onPressed: {
                            if (mouse.button == Qt.LeftButton && mouse.modifiers & Qt.ShiftModifier) {
                                mouse.accepted = true
                                parent.gesture.enabled = false

                                parent.selectionBR = undefined
                                parent.selectionTL = parent.toCoordinate(Qt.point(mouse.x, mouse.y))

                            }
                        }
                        onPositionChanged: {
                            if (mouse.buttons & Qt.LeftButton && mouse.modifiers & Qt.ShiftModifier) {
                                var crd = parent.toCoordinate(Qt.point(mouse.x, mouse.y))
                                parent.selectionBR = crd
                            }
                        }

                        onReleased: {
                            parent.gesture.enabled = true
                            // trigger Map Data fetching
                            if (tileMode.checked) {
                                utilities.requestSlippyTiles(parent.selectionTL,
                                                             parent.selectionBR,
                                                             zlslider.value,
                                                             zlslider.value)
                                if (rasterEnabled.checked) {
                                    mapFetcher.requestSlippyTiles(parent.selectionTL,
                                                                  parent.selectionBR,
                                                                  zlMapSlider.value,
                                                                  zlslider.value)
                                }
                            } else {
                                utilities.requestCoverage(parent.selectionTL,
                                                                 parent.selectionBR,
                                                                 zlslider.value,
                                                                 /*clip*/ clipInCoverage.checked)
                                if (rasterEnabled.checked) {
                                    mapFetcher.requestCoverage(parent.selectionTL,
                                                               parent.selectionBR,
                                                               zlslider.value,
                                                               /*clip*/ clipInCoverage.checked)
                                }
                            }
                        }
                    }

                    Column {
                        anchors {
                            left: parent.left
                            right: parent.right
                            bottom: parent.bottom
                            bottomMargin: 20
                            leftMargin: 20
                            rightMargin: 20
                        }

                        RowLayout {
                            spacing: 8
                            anchors.left: parent.left
                            anchors.right: parent.horizontalCenter
                            Text {
                                text: "Terrain"
                            }

                            QC2.Slider {
                                id: zlslider
                                from: 0
                                to: 15
                                value: 9
                                stepSize: 1
                                Layout.fillWidth: true
                                QC2.ToolTip {
                                    parent: zlslider.handle
                                    visible: zlslider.pressed
                                    text: zlslider.value.toFixed(0)
                                }
                            }
                        }

                        RowLayout {
                            spacing: 8
                            anchors.left: parent.left
                            anchors.right: parent.horizontalCenter
                            Text {
                                text: "Map"
                            }

                            QC2.Slider {
                                id: zlMapSlider
                                from: zlslider.value
                                to: 19
                                value: 9
                                stepSize: 1
                                Layout.fillWidth: true
                                QC2.ToolTip {
                                    parent: zlMapSlider.handle
                                    visible: zlMapSlider.pressed
                                    text: zlMapSlider.value.toFixed(0)
                                }
                            }
                        }
                    }


                }
            } // Item
        } // Tab
        Item {
//            title: "Terrain"
            Rectangle {
                anchors.fill: parent
                color: "green"
                TerrainViewer {
                    id: viewer
                    anchors.fill: parent
                    interactor: arcball
                    demUtilities: utilities
                    rasterFetcher: mapFetcher
                    joinTiles: joinTilesMenuItem.checked
                    elevationScale: elevationSlider.value
                    brightness: brightnessSlider.value
                    tessellationDirection: invertTessDirection.checked
                    lightDirection: shadingSphere.pos
                    offline: offline.checked

                    Component.onCompleted: {
                        arcball.modelTransformation = settings.modelTransformation
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.MidButton
                        onPressed: {
                            if (mouse.buttons & Qt.LeftButton)
                                arcball.pressed(Qt.point(mouse.x, mouse.y))
                            else if (mouse.buttons & Qt.MidButton)
                                arcball.midPressed(Qt.point(mouse.x, mouse.y))
                        }
                        onPositionChanged: arcball.moved(Qt.point(mouse.x, mouse.y))
                        onReleased: arcball.released()
                        onWheel: {
//                            if (wheel.modifiers & Qt.ControlModifier)
                            {
                                arcball.zoom(wheel.angleDelta.y / 120.0);
                            }
                        }
                    }
                }
                QC2.Slider {
                    id: elevationSlider
                    from: 1
                    to: 8000
                    value: 700
                    stepSize: 1
                    anchors {
                        left: parent.left
                        right: parent.right
                        bottom: parent.bottom
                        bottomMargin: 20
                        leftMargin: 20
                        rightMargin: 20
                    }

                    QC2.ToolTip {
                        parent: elevationSlider.handle
                        visible: elevationSlider.pressed
                        text: elevationSlider.value.toFixed(0)
                    }
                }
                Text {
                    anchors {
                        top: parent.top
                        topMargin: 10
                        left: parent.left
                        leftMargin: 10
                    }
                    function formatNumber (num) {
                        return num.toString().replace(/(\d)(?=(\d{3})+(?!\d))/g, "$1,")
                    }
                    text: "Triangles: " + formatNumber(viewer.triangles)
                }
                Text {
                    anchors {
                        top: parent.top
                        topMargin: 10
                        right: parent.right
                        rightMargin: 10
                    }
                    function formatNumber (num) {
                        return num.toString().replace(/(\d)(?=(\d{3})+(?!\d))/g, "$1,")
                    }
                    text: "Bytes: " + formatNumber(viewer.allocatedGraphicsBytes)
                }
            }
        }
    }

    QC2.Dialog {
        id: addProviderDialog
        modal: true
        title: "Add new provider template"
        width: 650
        height: 180
        padding: 16
        anchors.centerIn: parent

        function addVideo(u) {
            if (!utilities.isYoutubeVideoUrl(u)) {
                // Q_UNREACHABLE
                console.log("Wrong URL fed!")
                return;
            }
            if (utilities.isYoutubeShortsUrl(u)) {
                fileSystemModel.addEntry(utilities.getVideoID(u),
                                         "", // title
                                         "", // channel URL
                                         "", // channel Avatar url
                                         ""  // channel name
                                         )
            } else {
                fileSystemModel.addEntry(utilities.getVideoID(u),
                                         "", // title
                                         "", // channel URL
                                         "", // channel Avatar url
                                         "", // channel name
                                         1,
                                         0) // ToDo: make it update
            }
        }

        onAccepted: {
            var name = newProviderName.text
            var template = newProviderTemplate.text;
            newProviderName.clear()
            newProviderTemplate.clear()
            pushTemplate(name, template)
            close()
        }
        onRejected: {
            newProviderName.clear()
            newProviderTemplate.clear()
            close()
        }

        Rectangle {
            anchors {
                left: parent.left
                right: parent.right
                verticalCenter: parent.verticalCenter
            }
            height: newProviderName.height * 1.3
            color: "transparent"
            ColumnLayout {
                anchors.verticalCenter: parent.verticalCenter
                RowLayout {
                    Text {
                        Layout.preferredWidth: addProviderDialog.width * .45
                        text: "Name"
                        color: "white"
                    }
                    Text {
                        Layout.preferredWidth: addProviderDialog.width * .45
                        text: "URL Template"
                        color: "white"
                    }
                }

                RowLayout {
                    anchors.verticalCenter: parent.verticalCenter
                    QC2.TextField {
                        id: newProviderName
                        focus: true
                        Layout.preferredWidth: addProviderDialog.width * .45
                        selectByMouse: true
                        font.pixelSize: 14
                        cursorVisible: true

                    }
                    QC2.TextField {
                        id: newProviderTemplate
                        focus: true
                        Layout.preferredWidth: addProviderDialog.width * .45
                        selectByMouse: true
                        font.pixelSize: 14
                        cursorVisible: true

                    }
                }
            }
        }

        footer: QC2.DialogButtonBox {
            standardButtons: QC2.DialogButtonBox.Ok | QC2.DialogButtonBox.Cancel
        }
    } // addVideoDialog
}
