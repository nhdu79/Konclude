
message("Updating Konclude version from Git Revision.")

# Create our custom gitbuild target.
win32:gitbuild.commands = $${PWD}/WinGitBuildScript.bat
else:gitbuild.commands = $${PWD}/UnixGitBuildScript.sh
QMAKE_EXTRA_TARGETS += gitbuild

PRE_TARGETDEPS = gitbuild

message("Preparing source code of Konclude (embedded shared library build).")
TEMPLATE = lib
CONFIG += shared
TARGET = Konclude
DESTDIR = ./ReleaseEmbedded
QT += xml network concurrent
CONFIG += release warn_off c++11
DEFINES += QT_XML_LIB QT_NETWORK_LIB KONCLUDE_FORCE_ALL_DEBUG_DEACTIVATED KONCLUDE_COMPILE_EMBEDDED_INTERFACE
INCLUDEPATH += ./generatedfiles \
    ./GeneratedFiles/ReleaseEmbedded \
    ./Source \
	.
DEPENDPATH += .
MOC_DIR += ./GeneratedFiles/releaseembedded
OBJECTS_DIR += releaseembedded
UI_DIR += ./GeneratedFiles
RCC_DIR += ./GeneratedFiles

# macOS: qmake's default -install_name is a bare filename (e.g.
# "libKonclude.1.dylib"), which dyld only resolves via the system library
# paths -- an rpath baked into a consuming binary (as Tools/EmbeddedDriver's
# rebuild.sh does) is silently ignored unless the dependency itself is
# recorded as @rpath/-relative. Without this, every driver fails at startup
# with "Library not loaded: libKonclude.1.dylib".
macx: QMAKE_LFLAGS_SONAME = -Wl,-install_name,@rpath/

#Include file(s)
include(Konclude.pri)
