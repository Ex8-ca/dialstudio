QT += core gui network dbus

CONFIG += c++17 release link_pkgconfig
TARGET = dialstudio
TEMPLATE = app

include(src/src.pri)
HEADERS += src/cli.h
SOURCES += src/main.cpp src/cli.cpp
