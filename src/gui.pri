# The window: shared by the application and the test and screenshot binaries.
HEADERS += \
    $$PWD/backend.h \
    $$PWD/theme.h \
    $$PWD/translator.h \
    $$PWD/gui.h

SOURCES += \
    $$PWD/backend.cpp \
    $$PWD/theme.cpp \
    $$PWD/translator.cpp \
    $$PWD/gui.cpp

RESOURCES += $$PWD/resources.qrc
