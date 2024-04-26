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
import QtQuick.Dialogs 1.3 as QQD
import QtPositioning 5.15
import QtLocation 5.15
import Qt.labs.settings 1.1
import Qt.labs.platform 1.1
import DemViewer 1.0

QC2.ApplicationWindow {
    id: root
    width: 1404
    height: 700
    visible: true
    title: qsTr("DEM Viewer")
    objectName: "Application Window"

    Component.onCompleted: {
        if (root.urlTemplates.length == 0) {
            root.pushTemplate("osm", "https://tile.openstreetmap.org/{z}/{x}/{y}.png")
        }
    }

    onWidthChanged: arcball.setSize(Qt.size(width, height))
    onHeightChanged: arcball.setSize(Qt.size(width, height))

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
        property alias decimation: decimationSlider.value
        property alias rasterEnabled: rasterEnabled.checked
        property alias fastInteraction: fastInteractionMenuItem.checked
        property alias autoRefinement: autoRefinementMenuItem.checked
//        property alias forwardUncompressed: forwardUncompressed.checked
        property alias offline: offline.checked
        property alias invertTessDirection: invertTessDirection.checked
        property alias lightPos: shadingSphere.pos
        property alias joinTiles: joinTilesMenuItem.checked
        property alias astc: astc.checked
        property alias logging: logRequests.checked
        property alias autoStride: autoStrideMenuItem.checked
        property alias modelCenter: baseMap2.center
        property alias modelZoom: baseMap2.zoomLevel
        property alias modelBearing: baseMap2.bearing
        property alias modelTilt: baseMap2.tilt
        property alias geoReferencing: geoReferencedMenuItem.checked
        property alias demOpacity: alphaSlider.value
        property alias demCoalescing: demCoalescingSlider.value
        property int selectedProvider: 0
        property var modelTransformation
        property var splitViewState

        Component.onCompleted: {
            mapFetcher.forwardUncompressedTiles = false
//                    Qt.binding(function() { return settings.forwardUncompressed })
            splitView.restoreState(settings.splitViewState)
            if (windows) {
                fileName = StandardPaths.writableLocation(StandardPaths.AppDataLocation) + "/demviewer.ini"
            }
        }
        Component.onDestruction: settings.splitViewState = splitView.saveState()
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
                text: qsTr("Clear Graphics Data")
                onTriggered: viewer.reset()
            }
        }
        QC2.Menu {
            title: qsTr("Provider")
            Repeater {
                model: root.urlTemplates
                id: repeaterProviders
                onItemAdded: {
                    if (count > settings.selectedProvider
                            && itemAt(settings.selectedProvider) !== null)
                        itemAt(settings.selectedProvider).checked = true
                }

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
                            settings.selectedProvider = index

                            for (let i = 0; i < repeaterProviders.count; i++) {
                                if (i != index
                                    && repeaterProviders.itemAt(i) !== null) {
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
                            console.log("Delete ", repeaterProviders.itemAt(index).text)
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
                id: geoReferencedMenuItem
                text: qsTr("Geo Referencing")
                checkable: true
                checked: false
                hoverEnabled: true
                QC2.ToolTip.visible: hovered
                QC2.ToolTip.text: "Render the mesh on top of a map"
                QC2.ToolTip.delay: 300
            }
            QC2.MenuItem {
                id: joinTilesMenuItem
                text: qsTr("Join Tiles")
                visible: false
                height: 0
                checkable: true
                checked: true
                QC2.ToolTip.visible: hovered
                QC2.ToolTip.text: "Render connecting geometry between tiles"
                QC2.ToolTip.delay: 300
            }
            QC2.MenuItem {
                id: fastInteractionMenuItem
                text: qsTr("Fast Interaction")
                checkable: true
                checked: true
                visible: false
                height: 0
                hoverEnabled: true
                QC2.ToolTip.visible: hovered
                QC2.ToolTip.text: "Render a decimated mesh during interaction"
                QC2.ToolTip.delay: 300
            }
            QC2.MenuItem {
                id: autoRefinementMenuItem
                text: qsTr("Auto Refinement")
                checkable: true
                checked: false
                visible: false
                height: 0
                hoverEnabled: true
                QC2.ToolTip.visible: hovered
                QC2.ToolTip.text: "Render the mesh fully when interaction stops"
                QC2.ToolTip.delay: 300
            }
            QC2.MenuItem {
                id: invertTessDirection
                checkable: true;
                text: qsTr("Alt tessellation")
                hoverEnabled: true
                QC2.ToolTip.visible: hovered
                QC2.ToolTip.text: "Split mesh quads along the other diagonal"
                QC2.ToolTip.delay: 300
            }
            QC2.MenuItem {
                id: autoStrideMenuItem
                checkable: true;
                checked: true
                text: qsTr("Auto decimation")
                hoverEnabled: true
                QC2.ToolTip.visible: hovered
                QC2.ToolTip.text: "Automatically select decimation based on zoom"
                QC2.ToolTip.delay: 300
            }
            QC2.MenuItem {
                id: astc
                text: qsTr("ASTC compression")
                checkable: true
                checked: false
                enabled: astcSupported

                hoverEnabled: true
                QC2.ToolTip.visible: hovered
                QC2.ToolTip.text: "Texture compression reduces memory usage"
                QC2.ToolTip.delay: 300
            }
//            QC2.MenuItem {
//                id: forwardUncompressed
//                text: qsTr("Use uncompressed tiles")
//                checkable: true
//                checked: false
//                enabled: astc.checked

//                hoverEnabled: true
//                QC2.ToolTip.visible: hovered
//                QC2.ToolTip.text: "Use uncompressed textures as soon as they are available"
//                QC2.ToolTip.delay: 300
//            }
            RowLayout {
                spacing: 4
                width: parent.width
                Text {
                    text: "Decimation"
                    color: "white"
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: 2
                }

                QC2.Slider {
                    id: decimationSlider
                    from: 1
                    to: 13
                    stepSize: 1
                    value: 4
                    snapMode: QC2.Slider.SnapAlways
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    property int rate: Math.trunc(Math.pow(2, decimationSlider.value - 1))

                    QC2.ToolTip {
                        parent: decimationSlider.handle
                        visible: decimationSlider.pressed
                        text: decimationSlider.rate
                    }
                }
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
            RowLayout {
                spacing: 4
                width: parent.width
                Text {
                    text: "α"
                    color: "white"
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: 2
                }

                QC2.Slider {
                    id: alphaSlider
                    from: 0.0
                    to: 1.0
                    value: 1
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter

                    QC2.ToolTip {
                        parent: alphaSlider.handle
                        visible: alphaSlider.pressed
                        text: alphaSlider.value.toFixed(2)
                    }
                }
            }
            RowLayout {
                spacing: 4
                width: parent.width
                Text {
                    text: "DEM Coalescing"
                    color: "white"
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: 2
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        property bool hovered: false
                        onEntered: hovered = true
                        onExited: hovered = false
                        QC2.ToolTip {
                            visible: parent.hovered
                            text: "Coalesce DEM tiles into tiles these many zoom levels less"
                        }
                    }
                }

                QC2.Slider {
                    id: demCoalescingSlider
                    from: 0.0
                    to: 4.0
                    value: 0
                    stepSize: 1
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter

                    QC2.ToolTip {
                        parent: demCoalescingSlider.handle
                        visible: demCoalescingSlider.pressed
                        text: demCoalescingSlider.value.toFixed(2)
                    }
                }
            }
            QC2.Action {
                text: qsTr("Reset Scene")
                onTriggered: {
                    arcball.reset()
                    baseMap2.bearing = 0
                    baseMap2.tilt = 0
                    //baseMap2.zoomLevel = 10
                    //baseMap2.center = QtPositioning.coordinate(35,8)
                }
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
        QC2.Menu {
            title: qsTr("Misc")
            QC2.MenuItem {
                id: logNetworkRequests
                text: qsTr("Log Network Requests")
                checkable: true
                checked: false

                hoverEnabled: true
                QC2.ToolTip.visible: hovered
                QC2.ToolTip.text: "Log retrieved tile URLs to the console"
                QC2.ToolTip.delay: 300
            }
            QC2.MenuItem {
                id: logRequests
                text: qsTr("Log Requests")
                checkable: true
                checked: false

                hoverEnabled: true
                QC2.ToolTip.visible: hovered
                QC2.ToolTip.text: "Log requests to a JSON file, that can be loaded back"
                QC2.ToolTip.delay: 300
            }
            QC2.MenuItem {
                id: replayLog
                text: qsTr("Replay")
                checkable: false
                onClicked: fileDialogReplay.open()
            }
        }
    }

    QC2.SplitView {
        id: splitView
        anchors.fill: parent
        Item {
            QC2.SplitView.preferredWidth: parent.width * .5
            Item {
                anchors.fill: parent
                Map {
                    id: basemap
                    center: overlay.center
                    zoomLevel: overlay.zoomLevel
                    anchors.fill: parent
                    gesture.enabled: false
                    copyrightsVisible: false
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
                    copyrightsVisible: false

                    // Do not allow rotation, to avoid headaches
                    gesture.acceptedGestures: MapGestureArea.PanGesture | MapGestureArea.FlickGesture | MapGestureArea.PinchGesture
                    center: QtPositioning.coordinate(60.39, 5.32)
                    zoomLevel: 9

                    plugin: Plugin {
                        name: "itemsoverlay"
                    }

                    property var selectionPolygon: []
                    property var displayedSelectionPolygon: []

                    MapPolygon {
                        objectName: "selectionPolygon"
                        visible: path.length > 2
                        color: "transparent"
                        border.color: "red"
                        path: (overlay.displayedSelectionPolygon.length != 0)
                              ? overlay.displayedSelectionPolygon
                              : overlay.selectionPolygon
                        autoFadeIn: false
                    }



                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onPressed: {
                            tips.hide()
                            if (mouse.button == Qt.LeftButton && mouse.modifiers & Qt.ShiftModifier) {
                                mouse.accepted = true
                                parent.gesture.enabled = false
                                let selection = overlay.displayedSelectionPolygon
                                selection.push(
                                            overlay.toCoordinate(Qt.point(mouse.x, mouse.y)))
                                overlay.displayedSelectionPolygon = selection
                            } else if (mouse.button == Qt.LeftButton && mouse.modifiers & Qt.ControlModifier) {

                            } else if (mouse.button == Qt.RightButton) {
                                mouse.accepted = true
                                overlay.selectionPolygon = overlay.displayedSelectionPolygon
                                overlay.displayedSelectionPolygon = []
                                if (mouse.modifiers & Qt.ShiftModifier &&
                                    overlay.selectionPolygon.length > 2) {
                                    fireQuery()
                                } else {
                                    overlay.selectionPolygon = []
                                }
                            }
                        }

                        onReleased: {
                            parent.gesture.enabled = true
                        }

                        function fireQuery() {
                            var res;
                            if (tileMode.checked) {
                                let destZoom = Math.max(zlslider.value - settings.demCoalescing, 0)
                                res = demfetcher.requestSlippyTiles(parent.selectionPolygon,
                                                             zlslider.value,
                                                             destZoom)
                                console.log("Request ",res,"issued")
                                if (logRequests.checked)
                                    utilities.logRequest(demfetcher,
                                                         parent.selectionPolygon,
                                                         zlslider.value,
                                                         destZoom)
                                if (rasterEnabled.checked) {
                                    res = mapFetcher.requestSlippyTiles(
                                                                  parent.selectionPolygon,
                                                                  zlMapSlider.value,
                                                                  destZoom,
                                                                  false)
                                    console.log("Request ",res,"issued")
                                    if (logRequests.checked)
                                        utilities.logRequest(mapFetcher,
                                                             parent.selectionPolygon,
                                                             zlMapSlider.value,
                                                             destZoom)
                                }
                            } else {
                                res = demfetcher.requestCoverage(parent.selectionPolygon,
                                                                 zlslider.value,
                                                                 /*clip*/ clipInCoverage.checked)
                                console.log("Request ",res,"issued")
                                if (rasterEnabled.checked) {
                                    res = mapFetcher.requestCoverage(parent.selectionPolygon,
                                                               zlslider.value,
                                                               /*clip*/ clipInCoverage.checked)
                                    console.log("Request ",res,"issued")
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
                                property int maxNativeZL: 15
                                property real stepWidth: 1.0 / (to - from)
                                stepSize: 1
                                Layout.fillWidth: true

//                                background: Item {
//                                    id: zlslibg
//                                    x: zlslider.leftPadding
//                                    y: zlslider.topPadding + zlslider.availableHeight / 2. - height / 2.
//                                    width: parent.width
//                                    height: implicitHeight
//                                    anchors.alignWhenCentered: false

//                                    Rectangle {
//                                        id: bgSli
//                                        implicitWidth: 200
//                                        implicitHeight: 4
//                                        width: zlslider.availableWidth * zlslider.stepWidth * (zlslider.maxNativeZL)
//                                        height: implicitHeight
//                                        radius: 2
//                                        color: zlslider.handle.children[0].color
//                                        opacity: 0.4
//                                    }
//                                    Rectangle {
//                                        id: bgSliOver
//                                        anchors.left: bgSli.right
//                                        implicitWidth: 200
//                                        implicitHeight: 4
//                                        width: zlslider.availableWidth - bgSli.width
//                                        height: implicitHeight
//                                        radius: 2
//                                        color: Qt.rgba(1,0.2,0.2,1)
//                                        opacity: 0.7
//                                    }
//                                    Rectangle {
//                                        width: zlslider.visualPosition * zlslider.availableWidth
//                                        height: bgSli.height
//                                        color: zlslider.handle.children[0].color
//                                        radius: 2
//                                    }
//                                }

                                QC2.ToolTip {
                                    parent: zlslider.handle
                                    visible: zlslider.pressed
                                    text: zlslider.value.toFixed(0) + ((zlslider.value > 15) ? "(magnified)" : "")
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

                    Component.onCompleted: {
                        if (visible)
                            hiderTimer.start()
                    }
                    gesture.onPanStarted: {
                        tips.hide()
                    }
                    gesture.onPinchStarted: {
                        tips.hide()
                    }
                    Rectangle {
                        id: tips
                        Timer {
                            id: hiderTimer
                            repeat: false
                            interval: 4000
                            running: false
                            onTriggered: tips.hide()
                        }
                        function hide() {
                            tips.opacity = 0
                        }

                        anchors.centerIn: parent
                        color: Qt.rgba(0.55,0.6,0.6,0.65)
                        radius: 5
                        width: 550
                        height: 40
                        visible: opacity !== 0
                        Behavior on opacity {
                          NumberAnimation  { duration: 700 ; easing.type: Easing.OutQuad  }
                        }

                        Text {
                            anchors.centerIn: parent
                            text: "Shift + Left click to draw a map area. Shift + Right click to commit."
                            font.bold: true
                        }
                    }
                }
            } // Item
        } // Tab
        Item {
            Rectangle {
                anchors.fill: parent
                color: "green"

                Map {
                    id: baseMap2
                    visible: settings.geoReferencing
                    plugin: basemap.plugin
                    anchors.fill: parent
                    opacity: 0.85
                    copyrightsVisible: false
                    gesture.enabled: true
                    gesture.acceptedGestures: MapGestureArea.PanGesture | MapGestureArea.FlickGesture | MapGestureArea.PinchGesture | MapGestureArea.RotationGesture | MapGestureArea.TiltGesture
                }

                TerrainViewer {
                    id: viewer
                    opacity: settings.demOpacity
                    anchors.fill: parent
                    interactor: arcball
                    demFetcher: demfetcher
                    rasterFetcher: mapFetcher
                    joinTiles: true //joinTilesMenuItem.checked
                    elevationScale: elevationSlider.value
                    brightness: brightnessSlider.value
                    tessellationDirection: invertTessDirection.checked
                    lightDirection: shadingSphere.pos
                    offline: offline.checked
                    logRequests: logNetworkRequests.checked
                    astcEnabled: settings.astc && astcSupported
                    fastInteraction: fastInteractionMenuItem.checked
                    autoRefinement: autoStrideMenuItem.checked //autoRefinementMenuItem.checked
                    downsamplingRate: decimationSlider.rate
                    map: baseMap2
                    geoReferenced: settings.geoReferencing

                    Component.onCompleted: {
                        arcball.modelTransformation = settings.modelTransformation
                    }

                    onWidthChanged: updateArcball()
                    onHeightChanged: updateArcball()
                    function updateArcball() {
                        arcball.setSize(Qt.size(viewer.width, viewer.height))
                    }

                    MouseArea {
                        enabled: !settings.geoReferencing
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
                            arcball.zoom(wheel.angleDelta.y / 120.0);
                        }
                    }
                }
                QC2.Slider {
                    id: elevationSlider
                    from: 0.1
                    to: 20
                    value: 700
                    stepSize: 0.05
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
                        text: elevationSlider.value.toFixed(2)
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
                    Layout.alignment: Qt.AlignVCenter
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
    QQD.FileDialog {
        id: fileDialogReplay
        nameFilters: []
        title: "Please choose a replay json file"
        selectExisting: true
        selectFolder: false
        onAccepted: {
            fileDialogReplay.close()
            var path = String(fileDialogReplay.fileUrl)
            utilities.replay([mapFetcher, demfetcher], path)
        }
        onRejected: {
        }
    }
}
