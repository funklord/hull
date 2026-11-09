# gui/test/core_link_test.pro -- core_link against a real core, which
# test/gui_test.sh starts; built by `make check-gui`, with HULL_BUILD as
# gui.pro has it.

TEMPLATE = app
TARGET = core_link_test
QT -= gui
QT += core
CONFIG += c++17 console
CONFIG -= app_bundle

isEmpty(HULL_BUILD): error("core_link_test.pro: HULL_BUILD is not set; build with `make check-gui`")
DESTDIR = $$HULL_BUILD/test/gui
QMAKE_CXXFLAGS += -Wall -Wextra

ROOT = $$PWD/../..
INCLUDEPATH += $$PWD/../src $$ROOT/core $$ROOT/agent $$ROOT/sexpr $$HULL_BUILD/gen \
	$$ROOT/vendor/situ/runtime/c
LIBS += $$HULL_BUILD/libhull_client.a
PRE_TARGETDEPS += $$HULL_BUILD/libhull_client.a

SOURCES += core_link_test.cpp ../src/core_link.cpp
HEADERS += ../src/core_link.h
