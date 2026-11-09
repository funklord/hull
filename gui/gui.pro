# gui/gui.pro -- qmake project for hull-gui, hull's machine manager, the
# Qt Widgets frontend of project.md section 2.
#
# Built by the root Makefile's `make gui`, which builds what this links
# first and hands qmake two paths: HULL_BUILD, the build directory of the
# C (libhull_client.a and the generated headers), and, where vendor/qtty
# is checked out, QTTY_LIB_DIR, where libqtty.a was built. Not by a bare
# `make`, which builds hull and its agent and nothing around them.
#
# No QtQuick and no QML: hull's GUI is Qt Widgets, and qtty draws the same
# widgets on a terminal, which a scene graph would not survive.

TEMPLATE = app
TARGET = hull-gui
QT += widgets
CONFIG += c++17
CONFIG -= app_bundle

isEmpty(HULL_BUILD): error("gui.pro: HULL_BUILD is not set; build with `make gui` from the root")
DESTDIR = $$HULL_BUILD

# -Os, said, or qmake's -O2 stays; and the size rule's unreachable sections
# dropped at link.
QMAKE_CXXFLAGS_RELEASE -= -O2
QMAKE_CXXFLAGS_RELEASE += -Os
QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections -Wall -Wextra
QMAKE_LFLAGS += -Wl,--gc-sections

ROOT = $$PWD/..
INCLUDEPATH += $$ROOT/core $$ROOT/agent $$ROOT/sexpr $$HULL_BUILD/gen $$ROOT/vendor/situ/runtime/c
LIBS += $$HULL_BUILD/libhull_client.a
PRE_TARGETDEPS += $$HULL_BUILD/libhull_client.a
DEFINES += HULL_VERSION=\\\"$$cat($$ROOT/VERSION, singleline)\\\"

# qtty: the same window on a terminal. Optional, as the Makefile decides:
# without it, hull-gui is the window alone and says so when asked for --tui.
!isEmpty(QTTY_LIB_DIR):exists($$QTTY_LIB_DIR/libqtty.a) {
	include($$ROOT/vendor/qtty/qtty.pri)
	DEFINES += HULL_QTTY
	LIBS += -L$$QTTY_LIB_DIR -lqtty
	PRE_TARGETDEPS += $$QTTY_LIB_DIR/libqtty.a
	QT += dbus
}

SOURCES += \
	src/core_link.cpp \
	src/images_dialog.cpp \
	src/machine_list.cpp \
	src/main.cpp \
	src/main_window.cpp \
	src/new_machine_dialog.cpp \
	src/pools_dialog.cpp

HEADERS += \
	src/core_link.h \
	src/images_dialog.h \
	src/machine_list.h \
	src/main_window.h \
	src/new_machine_dialog.h \
	src/pools_dialog.h
