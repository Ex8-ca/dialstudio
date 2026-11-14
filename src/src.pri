# Sources shared by the application and the test binary (everything but main).
CONFIG += link_pkgconfig
PKGCONFIG += xkbcommon

HEADERS += \
    $$PWD/version.h \
    $$PWD/teacodec.h \
    $$PWD/protocol.h \
    $$PWD/shortcut.h \
    $$PWD/omarchybinds.h \
    $$PWD/actions.h \
    $$PWD/inputstate.h \
    $$PWD/config.h \
    $$PWD/profiles.h \
    $$PWD/bluezlink.h \
    $$PWD/ipc.h \
    $$PWD/daemon.h

SOURCES += \
    $$PWD/teacodec.cpp \
    $$PWD/protocol.cpp \
    $$PWD/shortcut.cpp \
    $$PWD/omarchybinds.cpp \
    $$PWD/actions.cpp \
    $$PWD/inputstate.cpp \
    $$PWD/config.cpp \
    $$PWD/profiles.cpp \
    $$PWD/bluezlink.cpp \
    $$PWD/ipc.cpp \
    $$PWD/daemon.cpp
