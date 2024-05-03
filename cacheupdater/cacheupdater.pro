TEMPLATE = app

include($$PWD/../arch_helper.pri)
#DESTDIR = $$clean_path($$PWD/bin/)
DESTDIR = $$clean_path($$PWD/bin/$${ARCH_PATH}/$${CONFIG_PATH}/$${TYPE_PATH}/$${QT_MAJOR_VERSION}.$${QT_MINOR_VERSION})
OBJECTS_DIR = $$DESTDIR/.obj
MOC_DIR = $$DESTDIR/.moc
RCC_DIR = $$DESTDIR/.rcc
UI_DIR = $$DESTDIR/.ui

QT += quick core gui qml widgets quickcontrols2 network sql remoteobjects
QT += positioning-private #for QDouble math
QT += location-private #for QGeoCameraTiles/Private

CONFIG += c++14
CONFIG += static
CONFIG += console
CONFIG -= app_bundle

include($$PWD/../mapfetcher/mapfetcher.pri)
include($$PWD/../astcencoder/astcencoder.pri)

HEADERS += sqlite_adapter.h
SOURCES += \
        main.cpp \
        sqlite_adapter.cpp

