INCLUDEPATH += $$PWD


include($$PWD/../arch_helper.pri)
#INCLUDEDIR = $$clean_path($$PWD/bin/$${ARCH_PATH}/$${CONFIG_PATH}/$${TYPE_PATH}/$${QT_MAJOR_VERSION}.$${QT_MINOR_VERSION})
INCLUDEDIR = $$clean_path($$PWD/bin/$${ARCH_PATH}/release/$${TYPE_PATH}/$${QT_MAJOR_VERSION}.$${QT_MINOR_VERSION})
#message(!!! $$INCLUDEDIR)
PRE_TARGETDEPS += $$INCLUDEDIR/libastcencoder.a
LIBS += -L$$INCLUDEDIR -lastcencoder
