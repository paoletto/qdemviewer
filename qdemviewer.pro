TEMPLATE = subdirs

SUBDIRS = \
        mapfetcher \
        demviewer \
        astcencoder \
        cacheupdater

OTHER_FILES += \
    mapfetcher/mapfetcher.pro \
    demviewer/demviewer.pro\
    astcencoder/astcencoder.pro\
    cacheupdater/cacheupdater.pro\
    arch_helper.pri\
    LICENSE\
    README.md
