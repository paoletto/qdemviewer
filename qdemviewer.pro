TEMPLATE = subdirs

SUBDIRS = \
        mapfetcher \
        demviewer \
        astcencoder \
        mapupdater

OTHER_FILES += \
    mapfetcher/mapfetcher.pro \
    demviewer/demviewer.pro\
    astcencoder/astcencoder.pro\
    mapupdater/mapupdater.pro\
    arch_helper.pri\
    LICENSE\
    README.md
