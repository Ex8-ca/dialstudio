QT += core gui network dbus testlib
CONFIG += c++17 testcase
TARGET = dialstudio_tests
TEMPLATE = app

INCLUDEPATH += ../src .
include(../src/src.pri)

HEADERS += fakebluez.h testmain.h
SOURCES += tests_main.cpp fakebluez.cpp \
    tests_protocol.cpp tests_inputstate.cpp tests_config.cpp tests_actions.cpp \
    tests_profiles.cpp tests_bluezlink.cpp tests_daemon.cpp
