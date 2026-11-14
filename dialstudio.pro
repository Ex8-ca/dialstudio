QT += core gui network dbus qml quick quickcontrols2 svg

CONFIG += c++17 release link_pkgconfig
TARGET = dialstudio
TEMPLATE = app

include(src/src.pri)
include(src/gui.pri)
HEADERS += src/cli.h
SOURCES += src/main.cpp src/cli.cpp
