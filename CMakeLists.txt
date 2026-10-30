cmake_minimum_required(VERSION 3.19)
project(sawfish VERSION 0.1.0 LANGUAGES CXX)
set(SAWFISH_VERSION "${PROJECT_VERSION}" CACHE STRING "Version shown by Sawfish")
if(NOT SAWFISH_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([-+][0-9A-Za-z.+-]+)?$")
    message(FATAL_ERROR "SAWFISH_VERSION must be a semantic version")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
include(CTest)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Distribution Qt plugins may have no reserved ELF RPATH. CMake's built-in
    # RPATH_SET cannot add one, whereas patchelf can create it during deployment.
    set(QT_DEPLOY_USE_PATCHELF ON)
endif()
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network Svg)
find_package(Protobuf CONFIG REQUIRED)
if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "Windows Unix sockets require the MSVC build of gRPC (MinGW is unsupported).")
    endif()
    find_package(gRPC 1.84 CONFIG REQUIRED)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX)
else()
    find_package(gRPC CONFIG REQUIRED)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
qt_standard_project_setup()

set(PROTO_FILES daemon/started_service.proto daemon/managed_service.proto
    experimental/boxdd/desktop_service.proto)
set(PROTO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated")
foreach(proto IN LISTS PROTO_FILES)
    string(REPLACE ".proto" "" stem "${proto}")
    list(APPEND PROTO_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/proto/${proto}")
    list(APPEND PROTO_SOURCES "${PROTO_OUTPUT}/${stem}.pb.cc" "${PROTO_OUTPUT}/${stem}.pb.h"
        "${PROTO_OUTPUT}/${stem}.grpc.pb.cc" "${PROTO_OUTPUT}/${stem}.grpc.pb.h")
endforeach()
add_custom_command(OUTPUT ${PROTO_SOURCES}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROTO_OUTPUT}"
    COMMAND protobuf::protoc --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
        --cpp_out=${PROTO_OUTPUT} --grpc_out=${PROTO_OUTPUT}
        --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin> ${PROTO_INPUTS}
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/cmake/namespace_proto.py" "${PROTO_OUTPUT}"
    DEPENDS ${PROTO_INPUTS} cmake/namespace_proto.py protobuf::protoc gRPC::grpc_cpp_plugin VERBATIM)
add_library(sawfish_proto STATIC ${PROTO_SOURCES})
target_include_directories(sawfish_proto PUBLIC "${PROTO_OUTPUT}")
target_link_libraries(sawfish_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

add_library(sawfish_core STATIC
    backend/coreupdater.cpp backend/coreupdater.h
    backend/processcpu.cpp backend/processcpu.h
    backend/rpcclient.cpp backend/rpcclient.h backend/workerprocess.cpp backend/workerprocess.h
    app/controller.cpp app/controller.h models/logmodel.cpp models/logmodel.h
    backend/connectionstate.cpp backend/connectionstate.h models/connectionmodel.cpp models/connectionmodel.h models/format.h
    models/profilestore.cpp models/profilestore.h models/groupmodel.cpp models/groupmodel.h
    backend/groupstate.h backend/daemonprocess.cpp backend/daemonprocess.h backend/ownedprocess.h)
target_include_directories(sawfish_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sawfish_core PUBLIC sawfish_proto Qt::Core Qt::Network)
target_compile_options(sawfish_core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>)
add_library(sawfish_widgets STATIC mainwindow.cpp mainwindow.h
    widgets/icons.cpp widgets/icons.h
    widgets/controls.cpp widgets/controls.h widgets/applicationicon.cpp widgets/applicationicon.h
    widgets/toast.cpp widgets/toast.h
    widgets/columnsettings.cpp widgets/columnsettings.h
    widgets/dashboard.cpp widgets/dashboard.h pages/connectionspage.cpp pages/connectionspage.h
    pages/homepage.cpp pages/homepage.h pages/profilespage.cpp pages/profilespage.h
    pages/logspage.cpp pages/logspage.h pages/settingspage.cpp pages/settingspage.h
    pages/proxiespage.cpp pages/proxiespage.h)
qt_add_resources(sawfish_widgets sawfish_icons
    PREFIX "/icons/heroicons" BASE "assets/heroicons"
    FILES assets/heroicons/plus.svg assets/heroicons/ellipsis-vertical.svg
    assets/heroicons/x-mark.svg assets/heroicons/pencil-square.svg
    assets/heroicons/arrow-path.svg
    assets/heroicons/pause.svg assets/heroicons/play.svg assets/heroicons/cog-6-tooth.svg
    assets/heroicons/document-duplicate.svg assets/heroicons/trash.svg
    assets/heroicons/chevron-down.svg assets/heroicons/chevron-right.svg
    assets/heroicons/check.svg assets/heroicons/LICENSE)
target_link_libraries(sawfish_widgets PUBLIC sawfish_core Qt::Widgets Qt::Svg)
target_compile_options(sawfish_widgets PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>)
qt_add_executable(sawfish WIN32 MACOSX_BUNDLE main.cpp)
target_link_libraries(sawfish PRIVATE sawfish_widgets)
target_compile_definitions(sawfish PRIVATE SAWFISH_VERSION="${SAWFISH_VERSION}")
target_compile_options(sawfish PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>)
if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    qt_add_executable(sawfish_desktop_tests tests/test_desktop.cpp)
    target_link_libraries(sawfish_desktop_tests PRIVATE sawfish_core Qt::Test)
    add_test(NAME sawfish_desktop COMMAND sawfish_desktop_tests)
    set_tests_properties(sawfish_desktop PROPERTIES TIMEOUT 60)
    qt_add_executable(sawfish_update_tests tests/test_updates.cpp)
    target_link_libraries(sawfish_update_tests PRIVATE sawfish_core Qt::Test)
    add_test(NAME sawfish_updates COMMAND sawfish_update_tests)
    set_tests_properties(sawfish_updates PROPERTIES TIMEOUT 180)
    qt_add_executable(sawfish_tests tests/test_core.cpp)
    # The model tester checks GUI role types, including QPixmap.
    target_link_libraries(sawfish_tests PRIVATE sawfish_core Qt::Gui Qt::Test)
    add_test(NAME sawfish_core COMMAND sawfish_tests)
    set_tests_properties(sawfish_core PROPERTIES TIMEOUT 45 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(sawfish_rpc_tests tests/test_rpc.cpp)
    target_link_libraries(sawfish_rpc_tests PRIVATE sawfish_core Qt::Test)
    add_test(NAME sawfish_rpc COMMAND sawfish_rpc_tests)
    set_tests_properties(sawfish_rpc PROPERTIES TIMEOUT 60)
    qt_add_executable(sawfish_ui_tests tests/test_ui.cpp)
    target_link_libraries(sawfish_ui_tests PRIVATE sawfish_widgets Qt::Test)
    add_test(NAME sawfish_ui COMMAND sawfish_ui_tests)
    set_tests_properties(sawfish_ui PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(sawfish_dragdrop_tests tests/test_dragdrop.cpp)
    target_link_libraries(sawfish_dragdrop_tests PRIVATE sawfish_widgets Qt::Test)
    add_test(NAME sawfish_dragdrop COMMAND sawfish_dragdrop_tests)
    set_tests_properties(sawfish_dragdrop PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=minimal")
    # These older suites exercise Linux run-mode authentication and /proc.
    # The desktop and update suites above exercise the native Windows path too.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    qt_add_executable(sawfish_backend_tests tests/test_backend.cpp)
    target_link_libraries(sawfish_backend_tests PRIVATE sawfish_core Qt::Test)
    add_test(NAME sawfish_backend COMMAND sawfish_backend_tests)
    set_tests_properties(sawfish_backend PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
    qt_add_executable(sawfish_lifecycle_tests tests/test_lifecycle.cpp)
    target_link_libraries(sawfish_lifecycle_tests PRIVATE sawfish_core Qt::Test)
    add_test(NAME sawfish_lifecycle COMMAND sawfish_lifecycle_tests)
    set_tests_properties(sawfish_lifecycle PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
    endif()
endif()
install(TARGETS sawfish BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
set(SAWFISH_ROSTRA_EXECUTABLE "" CACHE FILEPATH "Matching desktop core to bundle beside Sawfish")
if(SAWFISH_ROSTRA_EXECUTABLE)
    install(PROGRAMS "${SAWFISH_ROSTRA_EXECUTABLE}" DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME "rostra-daemon${CMAKE_EXECUTABLE_SUFFIX}")
endif()
install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/sawfish)
install(FILES proto/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/sawfish RENAME Rostra-protocols-LICENSE)
install(FILES assets/heroicons/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/sawfish RENAME Heroicons-LICENSE)
qt_generate_deploy_app_script(TARGET sawfish OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})
