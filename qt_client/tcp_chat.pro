QT += widgets network
CONFIG += c++17
QMAKE_CXX = g++-13
TARGET = qt_chat
INCLUDEPATH += ../common
SOURCES += main.cpp session.cpp store.cpp login_window.cpp main_window.cpp \
    ../common/protocol.cpp ../common/e2e.cpp ../common/aes.cpp \
    ../common/log.cpp
HEADERS += session.h store.h login_window.h main_window.h
LIBS += -lssl -lcrypto -lpthread /usr/lib/x86_64-linux-gnu/libsqlite3.so.0
