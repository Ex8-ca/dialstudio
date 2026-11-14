# Offscreen harness: drives the real QML against a running daemon and saves screenshots.
# Not part of bin/test (it needs a daemon); run it by hand, see docs/gui-test.md.
QT += core gui network dbus qml quick quickcontrols2 svg testlib
CONFIG += c++17 link_pkgconfig
TARGET = dialstudio_shots
TEMPLATE = app
INCLUDEPATH += ../../src
include(../../src/src.pri)
include(../../src/gui.pri)
SOURCES += shots.cpp
