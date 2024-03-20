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

INCLUDEPATH += $$PWD/astc-encoder

#Input
HEADERS += mapfetcher.h mapfetcher_p.h \
    networksqlitecache_p.h
SOURCES += mapfetcher.cpp \
    networksqlitecache.cpp

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

OTHER_FILES += mapfetcher.pri
