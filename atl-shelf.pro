QT += widgets network
CONFIG += c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = atl-shelf
SOURCES += src/main.cpp
RESOURCES += data/fdroid-signing-key.qrc

exists(/usr/include/KF6/KWindowSystem/KWindowSystem) {
    INCLUDEPATH += /usr/include/KF6/KWindowSystem
    LIBS += -lKF6WindowSystem
    DEFINES += HAVE_KWINDOWSYSTEM
}
