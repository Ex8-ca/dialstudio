QT += core gui network dbus testlib
CONFIG += c++17 testcase
TARGET = dialstudio_tests
TEMPLATE = app

INCLUDEPATH += ../src
include(../src/src.pri)

SOURCES += dialstudio_tests.cpp
