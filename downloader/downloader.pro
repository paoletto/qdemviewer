TEMPLATE = app

QT += quick core gui qml widgets quickcontrols2 network sql
QT += positioning-private
QT += location-private

CONFIG += c++14
CONFIG += static

include($$PWD/../mapfetcher/mapfetcher.pri)
include($$PWD/../astcencoder/astcencoder.pri)

SOURCES += \
        main.cpp

qml.files = $$files($$PWD/*.qml)
qml.base = $$PWD/
qml.prefix = /

providers.files = $$files($$PWD/providers/*)
providers.base = $$PWD/
providers.prefix = /

RESOURCES += qml providers
