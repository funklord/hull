# gui/test/screen_window_test.pro -- screen_window against a running vm's
# D-Bus display, which test/screen_test.sh starts; built by `make
# check-screen`, with HULL_BUILD as gui.pro has it.

TEMPLATE = app
TARGET = screen_window_test
QT += widgets openglwidgets
CONFIG += c++17 console link_pkgconfig
CONFIG -= app_bundle
PKGCONFIG += gio-unix-2.0 egl

isEmpty(HULL_BUILD): error("screen_window_test.pro: HULL_BUILD is not set; build with `make check-screen`")
DESTDIR = $$HULL_BUILD/test/screen-build
QMAKE_CXXFLAGS += -Wall -Wextra
INCLUDEPATH += $$PWD/../src

SOURCES += screen_window_test.cpp ../src/screen_window.cpp ../src/dmabuf_view.cpp
HEADERS += ../src/screen_window.h ../src/dmabuf_view.h
