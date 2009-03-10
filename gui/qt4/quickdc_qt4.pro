CONFIG += qt debug warn_on
QT += webkit network

INCLUDEPATH += ../../core
LIBS += ../../core/build/libquickdc.a
LIBS += -lbz2
LIBS += -lssl
LIBS += -lsamurai

HEADERS = mainview.h chatwidget.h hubview.h userwidget.h widgetlist.h eventloop.h socketmonitor.h scheduler.h
SOURCES = mainview.cpp chatwidget.cpp hubview.cpp userwidget.cpp widgetlist.cpp main.cpp eventloop.cpp socketmonitor.cpp scheduler.cpp
