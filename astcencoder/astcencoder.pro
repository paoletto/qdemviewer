TEMPLATE = lib
CONFIG += staticlib
win32:  TARGET = libastcencoder

include($$PWD/../arch_helper.pri)
DESTDIR = $$clean_path($$PWD/bin/$${ARCH_PATH}/$${CONFIG_PATH}/$${TYPE_PATH}/$${QT_MAJOR_VERSION}.$${QT_MINOR_VERSION})
OBJECTS_DIR = $$DESTDIR/.obj
MOC_DIR = $$DESTDIR/.moc
RCC_DIR = $$DESTDIR/.rcc
UI_DIR = $$DESTDIR/.ui

QT += core sql
QT += gui-private # TODO: make this one conditional together with ASTC support (and related inclusions below)

CONFIG += c++14


INCLUDEPATH += $$PWD/astc-encoder

CONFIG(debug, debug|release) {
    message($$DESTDIR)
} else {
    #DEFINES += "ASTCENC_ISA_AVX2=ON"
    #DEFINES += "ASTCENC_NEON=0"
    #DEFINES += "ASTCENC_F16C=1"
    DEFINES += "ASTCENC_SSE=42"
    DEFINES += "ASTCENC_AVX=2"
    DEFINES += "ASTCENC_POPCNT=1"
    QMAKE_FLAGS_RELEASE += -O3

    win32: {
        QMAKE_CXXFLAGS_RELEASE += /Ox
        QMAKE_FLAGS_RELEASE += /arch:AVX2
        QMAKE_CXXFLAGS_RELEASE += /arch:AVX2
    } else {
        QMAKE_CXXFLAGS_RELEASE += -O3
        QMAKE_FLAGS_RELEASE += -mavx2 -msse4.2 -msse4.1 -mssse3 -msse3 -msse2 -msse
        QMAKE_CXXFLAGS_RELEASE += -mavx2 -msse4.2 -msse4.1 -mssse3 -msse3 -msse2 -msse
        QMAKE_CXXFLAGS += "-fno-sized-deallocation"
    }
}

#Input
HEADERS +=  $$files(*.h)

SOURCES += $$files(*.cpp)

# TODO: figure proper compilesettings to build these
# in order to enable all optimizations/hw acceleration
#astc-encoder
HEADERS += $$PWD/astc-encoder/astcenc.h
SOURCES +=  $$PWD/astc-encoder/astcenc_averages_and_directions.cpp \
            $$PWD/astc-encoder/astcenc_block_sizes.cpp \
            $$PWD/astc-encoder/astcenc_color_quantize.cpp \
            $$PWD/astc-encoder/astcenc_color_unquantize.cpp \
            $$PWD/astc-encoder/astcenc_compress_symbolic.cpp \
            $$PWD/astc-encoder/astcenc_compute_variance.cpp \
            $$PWD/astc-encoder/astcenc_decompress_symbolic.cpp \
            $$PWD/astc-encoder/astcenc_diagnostic_trace.cpp \
            $$PWD/astc-encoder/astcenc_entry.cpp \
            $$PWD/astc-encoder/astcenc_find_best_partitioning.cpp \
            $$PWD/astc-encoder/astcenc_ideal_endpoints_and_weights.cpp \
            $$PWD/astc-encoder/astcenc_image.cpp \
            $$PWD/astc-encoder/astcenc_integer_sequence.cpp \
            $$PWD/astc-encoder/astcenc_mathlib.cpp \
            $$PWD/astc-encoder/astcenc_mathlib_softfloat.cpp \
            $$PWD/astc-encoder/astcenc_partition_tables.cpp \
            $$PWD/astc-encoder/astcenc_percentile_tables.cpp \
            $$PWD/astc-encoder/astcenc_pick_best_endpoint_format.cpp \
            $$PWD/astc-encoder/astcenc_quantization.cpp \
            $$PWD/astc-encoder/astcenc_symbolic_physical.cpp \
            $$PWD/astc-encoder/astcenc_weight_align.cpp \
            $$PWD/astc-encoder/astcenc_weight_quant_xfer_tables.cpp

OTHER_FILES += astcencoder.pri
