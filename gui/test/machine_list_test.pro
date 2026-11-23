# gui/test/machine_list_test.pro -- machine_list against the real hull
# plan, which test/gui_test.sh gives it; built by `make check-gui`, with
# HULL_BUILD as gui.pro has it.

TEMPLATE = app
TARGET = machine_list_test
QT -= gui
QT += core
CONFIG += c++17 console
CONFIG -= app_bundle

isEmpty(HULL_BUILD): error("machine_list_test.pro: HULL_BUILD is not set; build with `make check-gui`")
DESTDIR = $$HULL_BUILD/test/gui
# The sources under test compiled as gui.pro ships them, -Os and the
# size rule's sections: qmake's default -O2 would test other code.
QMAKE_CXXFLAGS_RELEASE -= -O2
QMAKE_CXXFLAGS_RELEASE += -Os
QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections -Wall -Wextra
QMAKE_LFLAGS += -Wl,--gc-sections
INCLUDEPATH += $$PWD/../src

SOURCES += machine_list_test.cpp ../src/machine_list.cpp
HEADERS += ../src/machine_list.h
