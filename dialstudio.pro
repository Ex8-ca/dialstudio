QT += core gui network dbus

CONFIG += c++17 release link_pkgconfig
TARGET = dialstudio
TEMPLATE = app
DEFINES += DIALSTUDIO_VERSION=\\\"0.0.1\\\"

include(src/src.pri)
SOURCES += src/main.cpp
