TEMPLATE = subdirs

SUBDIRS = \
        mapfetcher \
        demviewer \
        astcencoder

OTHER_FILES += \
    mapfetcher/mapfetcher.pro \
    demviewer/demviewer.pro\
    astcencoder/astcencoder.pro\
    arch_helper.pri\
    LICENSE\
    README.md
