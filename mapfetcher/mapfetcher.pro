TEMPLATE = lib
CONFIG += staticlib

include($$PWD/../arch_helper.pri)
DESTDIR = $$clean_path($$PWD/bin/$${ARCH_PATH}/$${CONFIG_PATH}/$${TYPE_PATH}/$${QT_MAJOR_VERSION}.$${QT_MINOR_VERSION})
OBJECTS_DIR = $$DESTDIR/.obj
MOC_DIR = $$DESTDIR/.moc
RCC_DIR = $$DESTDIR/.rcc
UI_DIR = $$DESTDIR/.ui

QT += core gui network network-private sql
QT += positioning-private #for QDouble math
QT += location-private #for QGeoCameraTiles/Private

CONFIG += c++14
QMAKE_CXXFLAGS += "-fno-sized-deallocation"

#Input
HEADERS += mapfetcher.h mapfetcher_p.h \
    networksqlitecache_p.h
SOURCES += mapfetcher.cpp \
    networksqlitecache.cpp

OTHER_FILES += mapfetcher.pri
