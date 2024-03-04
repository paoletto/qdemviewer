TEMPLATE = subdirs

SUBDIRS = \
        mapfetcher \
        demviewer \
        astcencoder \
        cacheupdater \
	downloader

OTHER_FILES += \
    mapfetcher/mapfetcher.pro \
    demviewer/demviewer.pro\
    astcencoder/astcencoder.pro\
    cacheupdater/cacheupdater.pro\
    arch_helper.pri\
    LICENSE\
    README.md
