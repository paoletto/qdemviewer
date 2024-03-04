TEMPLATE = app

include($$PWD/../arch_helper.pri)
DESTDIR = $$clean_path($$PWD/bin/$${ARCH_PATH}/$${CONFIG_PATH}/$${TYPE_PATH}/$${QT_MAJOR_VERSION}.$${QT_MINOR_VERSION})
OBJECTS_DIR = $$DESTDIR/.obj
MOC_DIR = $$DESTDIR/.moc
RCC_DIR = $$DESTDIR/.rcc
UI_DIR = $$DESTDIR/.ui


QT += quick core gui qml widgets quickcontrols2 network sql
QT += positioning-private
QT += location-private

CONFIG += c++14
CONFIG += static

include($$PWD/../mapfetcher/mapfetcher.pri)
include($$PWD/../astcencoder/astcencoder.pri)

#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000

HEADERS += shaders.h

SOURCES += \
        main.cpp

RESOURCES += qml.qrc
QMAKE_CXXFLAGS += "-fno-sized-deallocation"

QML_IMPORT_PATH =

QML_DESIGNER_IMPORT_PATH =

qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
