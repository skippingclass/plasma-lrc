# SPDX-FileCopyrightText: 2026 pirkov
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

project(plasma-lrc VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Qt6::Widgets is pulled in by Plasma::Applet, which needs QAction.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus Gui Widgets Qml)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6Config REQUIRED)
find_package(KF6I18n REQUIRED)

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
    spicylyrics.cpp
    spicylyrics.h
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
    KF6::I18n
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
