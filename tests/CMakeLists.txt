find_package(Threads REQUIRED)
add_executable(barista-tests
    barista_tests.cpp
    ../src/frontend/qt_sdl/barista/Connector.cpp)
target_include_directories(barista-tests PRIVATE ../src/frontend/qt_sdl)
target_link_libraries(barista-tests PRIVATE Threads::Threads)
add_test(NAME barista-connector COMMAND barista-tests)
set_tests_properties(barista-connector PROPERTIES TIMEOUT 30)

# This driver automates the real executable in an isolated Xvfb display. It is
# never linked into or installed with melonDS.
if (BUILD_QT_SDL AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if (USE_QT6)
        find_package(Qt6 COMPONENTS Widgets REQUIRED)
        set(BARISTA_TEST_QT Qt6::Widgets)
    else()
        find_package(Qt5 COMPONENTS Widgets REQUIRED)
        set(BARISTA_TEST_QT Qt5::Widgets)
    endif()
    add_library(barista-qt-driver SHARED barista/qt_driver.cpp)
    target_link_libraries(barista-qt-driver PRIVATE ${BARISTA_TEST_QT} ${CMAKE_DL_LIBS})
endif()
