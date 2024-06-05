TEMPLATE = lib
CONFIG += staticlib

win32: TARGET = libmapfetcher

include($$PWD/../arch_helper.pri)
DESTDIR = $$clean_path($$PWD/bin/$${ARCH_PATH}/$${CONFIG_PATH}/$${TYPE_PATH}/$${QT_MAJOR_VERSION}.$${QT_MINOR_VERSION})
OBJECTS_DIR = $$DESTDIR/.obj
MOC_DIR = $$DESTDIR/.moc
RCC_DIR = $$DESTDIR/.rcc
UI_DIR = $$DESTDIR/.ui

QT += core gui network network-private sql
QT += positioning-private #for QDouble math
QT += location-private #for QGeoCameraTiles/Private
QT += gui-private # TODO: make this one conditional together with ASTC support (and related inclusions below)

CONFIG += c++14
!win32: {
    QMAKE_CXXFLAGS += "-fno-sized-deallocation"
}

mapfetcher_res.files = $$PWD/assets/white256.png \
                       $$PWD/assets/white256_8x8.astc \
                       $$PWD/assets/white128_8x8.astc \
                       $$PWD/assets/white64_8x8.astc \
                       $$PWD/assets/white32_8x8.astc \
                       $$PWD/assets/white16_8x8.astc \
                       $$PWD/assets/white8_8x8.astc \
                       $$PWD/assets/transparent256_8x8.astc \
                       $$PWD/assets/transparent128_8x8.astc \
                       $$PWD/assets/transparent64_8x8.astc \
                       $$PWD/assets/transparent32_8x8.astc \
                       $$PWD/assets/transparent16_8x8.astc \
                       $$PWD/assets/transparent8_8x8.astc
mapfetcher_res.base = $$PWD/assets
mapfetcher_res.prefix = /

RESOURCES += mapfetcher_res

INCLUDEPATH += $$PWD/astc-encoder

#Input
HEADERS +=  $$files(*.h)

SOURCES += $$files(*.cpp)

include($$PWD/../astcencoder/astcencoder.pri)

OTHER_FILES += mapfetcher.pri
