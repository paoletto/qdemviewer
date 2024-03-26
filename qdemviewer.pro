TEMPLATE = subdirs

SUBDIRS = \
        mapfetcher \
        demviewer \
        #tileserver

OTHER_FILES += \
    mapfetcher/mapfetcher.pro \
    demviewer/demviewer.pro\
    tileserver/tileserver.pro\
    arch_helper.pri\
    LICENSE\
    README.md
