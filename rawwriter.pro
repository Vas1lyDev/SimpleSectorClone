TEMPLATE = app
CONFIG += console c++17
TARGET = RawWriter
QT += core
SOURCES += main.cpp\
           diskio.cpp
HEADERS += diskio.h

win32 {
    win32:CONFIG(release, debug|release): DESTDIR = $$OUT_PWD/release
    win32:CONFIG(debug,   debug|release): DESTDIR = $$OUT_PWD/debug

    WINDEPLOYQT = $$shell_path($$[QT_INSTALL_BINS]/windeployqt.exe)
    DESTDIR_SH  = $$shell_path($$DESTDIR)
    APP_EXE_SH  = $$shell_path($$DESTDIR/$${TARGET}.exe)  # <- именно $${TARGET}.exe

    # Сбросить всё, что добавлялось ранее, и оставить только одну команду
    QMAKE_POST_LINK =
    QMAKE_POST_LINK += \"$$WINDEPLOYQT\" --verbose 2 --compiler-runtime --no-translations --dir \"$$DESTDIR_SH\" \"$$APP_EXE_SH\"
}
