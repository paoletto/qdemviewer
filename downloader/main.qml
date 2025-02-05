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
import QtQuick.Dialogs 1.3 as QQD

Window {
    width: 1280
    height: 960
    visible: true
    title: qsTr("Map Coverage Downloader")

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

                    PluginParameter { name: "osm.useragent"; value: "My great Qt OSM application" }
                    PluginParameter {
                        name: "osm.mapping.providersrepository.address"
                        value: "qrc:/providers"
                    }
                    PluginParameter {
                        name: "osm.mapping.cache.disk.cost_strategy"
                        value: "unitary"
                    }
                    PluginParameter {
                        name: "osm.mapping.cache.memory.cost_strategy"
                        value: "unitary"
                    }
                    PluginParameter {
                        name: "osm.mapping.cache.disk.size"
                        value: 1000
                    }
                    PluginParameter {
                        name: "osm.mapping.cache.memory.size"
                        value: 30
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
                id: maQuery
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
                            fileDialogSave.open()
                        } else {
                            overlay.selectionPolygon = overlay.displayedSelectionPolygon = []
                        }
                    }
                }

                onReleased: {
                    parent.gesture.enabled = true
                }

                function fireQuery(path) {
                    var res;
                    utilities.download(path,
                                        parent.selectionPolygon,
                                        zlslider.value,
                                        zlMapSlider.value)
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


    QQD.FileDialog {
        id: fileDialogSave
        nameFilters: []
        title: "Please choose a directory to store coverages"
        selectExisting: true
        selectFolder: true

        onAccepted: {
            fileDialogSave.close()
            let path = String(fileDialogSave.fileUrl)
            maQuery.fireQuery(path)
        }
        onRejected: {
        }
    }

}
