# SPDX-FileCopyrightText: 2026 pirkov
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

project(plasma-lrc VERSION 1.7.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Qt6::Widgets is pulled in by Plasma::Applet, which needs QAction.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus Gui Widgets Qml Network)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6Config REQUIRED)
find_package(KF6I18n REQUIRED)
find_package(ZLIB REQUIRED)

# libPlasma is looked up directly instead of through its CMake package, because
# that one insists on extra-cmake-modules being installed.
find_library(LRC_PLASMA_LIBRARY NAMES Plasma REQUIRED)
find_path(LRC_PLASMA_INCLUDE_DIR NAMES Plasma/Applet PATH_SUFFIXES Plasma REQUIRED)

include(GNUInstallDirs)

# The KDEInstallDirs module of extra-cmake-modules is not required here, the two
# directories that matter are easy enough to compute by hand (and stay
# overridable from the command line).
if(NOT KDE_INSTALL_DATADIR)
    set(KDE_INSTALL_DATADIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}")
endif()
if(NOT KDE_INSTALL_PLUGINDIR)
    set(KDE_INSTALL_PLUGINDIR "${CMAKE_INSTALL_LIBDIR}/qt6/plugins")
endif()

set(PLUGIN_ID "org.kde.plasma.lrc")

add_library(${PLUGIN_ID} SHARED
    lrcapplet.cpp
    lrcapplet.h
    lrcparser.cpp
    lrcparser.h
    spicylyrics.cpp
    spicylyrics.h
    lrclib.cpp
    lrclib.h
    trackname.cpp
    trackname.h
    playermeta.cpp
    playermeta.h
    ttmlparser.cpp
    ttmlparser.h
    lyrictypes.h
    lyricsprovider.h
    spicylyricsprovider.cpp
    spicylyricsprovider.h
    lrclibprovider.cpp
    lrclibprovider.h
    amllprovider.cpp
    amllprovider.h
    lrcredprovider.cpp
    lrcredprovider.h
    kugouprovider.cpp
    kugouprovider.h
    neteaseprovider.cpp
    neteaseprovider.h
    unisonprovider.cpp
    unisonprovider.h
    qqmusicprovider.cpp
    qqmusicprovider.h
    localprovider.cpp
    localprovider.h
    customprovider.cpp
    customprovider.h
    lyricspipelinemanager.cpp
    lyricspipelinemanager.h
    zlibcodec.cpp
    zlibcodec.h
    krcparser.cpp
    krcparser.h
    yrcparser.cpp
    yrcparser.h
    qrcdecoder.cpp
    qrcdecoder.h
)

set_target_properties(${PLUGIN_ID} PROPERTIES
    PREFIX ""
    EXPORT_NAME ${PLUGIN_ID}
)

target_link_libraries(${PLUGIN_ID} PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Widgets
    Qt6::Network
    Qt6::Qml
    KF6::CoreAddons
    KF6::ConfigCore
    KF6::ConfigQml
    KF6::I18n
    ZLIB::ZLIB
    ${LRC_PLASMA_LIBRARY}
)

target_include_directories(${PLUGIN_ID} PRIVATE ${LRC_PLASMA_INCLUDE_DIR})

install(TARGETS ${PLUGIN_ID}
    DESTINATION "${KDE_INSTALL_PLUGINDIR}/plasma/applets"
)

# Plasma discovers applets by reading the metadata that moc embeds into the
# shared object from K_PLUGIN_METADATA(... FILE "metadata.json") below, so
# metadata.json has to sit next to the sources.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json"
    DESTINATION "${KDE_INSTALL_DATADIR}/plasma/plasmoids/${PLUGIN_ID}"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/contents/"
    DESTINATION "${KDE_INSTALL_DATADIR}/plasma/plasmoids/${PLUGIN_ID}/contents"
)

# Off by default: the parser tests need no Plasma, only Qt, and they are the
# only way to catch a response shape that quietly turns into "no lyrics".
option(PLASMA_LRC_TESTS "Build the lyrics parser tests" OFF)
if(PLASMA_LRC_TESTS)
    add_executable(spicy-parser-test
        tests/spicy-parser-test.cpp
        lrcparser.cpp
        lrcparser.h
        spicylyrics.cpp
        spicylyrics.h
        lrclib.cpp
        lrclib.h
        trackname.cpp
        trackname.h
        playermeta.cpp
        playermeta.h
    )
    target_link_libraries(spicy-parser-test PRIVATE Qt6::Core Qt6::Network Qt6::DBus)
    enable_testing()
    add_test(NAME spicy-parser COMMAND spicy-parser-test)

    add_executable(ttml-parser-test
        tests/ttml-parser-test.cpp
        ttmlparser.cpp
        ttmlparser.h
    )
    target_link_libraries(ttml-parser-test PRIVATE Qt6::Core)
    add_test(NAME ttml-parser COMMAND ttml-parser-test)

    add_executable(codecs-test
        tests/codecs-test.cpp
        zlibcodec.cpp
        zlibcodec.h
        krcparser.cpp
        krcparser.h
        yrcparser.cpp
        yrcparser.h
        qrcdecoder.cpp
        qrcdecoder.h
        ttmlparser.cpp
        ttmlparser.h
    )
    target_link_libraries(codecs-test PRIVATE Qt6::Core ZLIB::ZLIB)
    add_test(NAME codecs COMMAND codecs-test)

    add_executable(pipeline-test
        tests/pipeline-test.cpp
        lyricspipelinemanager.cpp
        lyricspipelinemanager.h
        lyricsprovider.h
        localprovider.cpp
        localprovider.h
        lrcparser.cpp
        lrcparser.h
        ttmlparser.cpp
        ttmlparser.h
    )
    target_link_libraries(pipeline-test PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME pipeline COMMAND pipeline-test)

    foreach(client IN ITEMS spicy lrclib)
        add_executable(${client}-client-test
            tests/${client}-client-test.cpp
            lrcparser.cpp spicylyrics.cpp lrclib.cpp trackname.cpp
        )
        target_link_libraries(${client}-client-test PRIVATE Qt6::Core Qt6::Network)
        add_test(NAME ${client}-client COMMAND ${client}-client-test)
    endforeach()

    add_executable(applet-sync-test tests/applet-sync-test.cpp)
    target_include_directories(applet-sync-test PRIVATE ${LRC_PLASMA_INCLUDE_DIR})
    target_link_libraries(applet-sync-test PRIVATE ${PLUGIN_ID}
        Qt6::Widgets Qt6::Qml Qt6::DBus Qt6::Network KF6::CoreAddons KF6::ConfigCore ${LRC_PLASMA_LIBRARY})
    add_test(NAME applet-sync COMMAND applet-sync-test)
    set_tests_properties(applet-sync PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
