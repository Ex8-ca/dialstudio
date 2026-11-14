QT += core gui network dbus testlib qml quick quickcontrols2 svg
CONFIG += c++17 testcase
TARGET = dialstudio_tests
TEMPLATE = app

INCLUDEPATH += ../src .
include(../src/src.pri)
include(../src/gui.pri)

HEADERS += fakebluez.h testmain.h
SOURCES += tests_main.cpp fakebluez.cpp \
    tests_protocol.cpp tests_inputstate.cpp tests_config.cpp tests_actions.cpp \
    tests_profiles.cpp tests_bluezlink.cpp tests_daemon.cpp tests_omarchybinds.cpp tests_appscan.cpp tests_presets.cpp tests_backend.cpp

DEFINES += FIXTURE_DIR=\\\"$$PWD/fixtures\\\"
