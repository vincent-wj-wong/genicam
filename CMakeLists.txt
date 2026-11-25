cmake_minimum_required(VERSION 3.16)
project(genicam VERSION 0.1.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(GC_BUILD_DEMOS "Build the GigE Vision demos" ON)
set(GC_OSAL_SOURCE "" CACHE FILEPATH "Replacement for the built-in OSAL source")
set(GC_OSAL_LIBRARIES "" CACHE STRING "Libraries for the replacement OSAL source")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(EXPAT REQUIRED)
find_package(ZLIB REQUIRED)

add_library(genicam STATIC
	src/core/device.c
	src/genapi/feature.c
	src/genapi/formula.c
	src/genapi/xml.c
	src/genapi/zip.c
	src/pfnc/pfnc.c
	src/transport/gige/client.c
	src/transport/gige/server.c
	src/transport/gige/stream.c
)
add_library(genicam::genicam ALIAS genicam)
target_include_directories(genicam
	PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
	       "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
	PRIVATE src
)
target_link_libraries(genicam PRIVATE EXPAT::EXPAT ZLIB::ZLIB)
set(GC_FIND_THREADS FALSE)
if(GC_OSAL_SOURCE)
	target_sources(genicam PRIVATE "${GC_OSAL_SOURCE}")
	target_link_libraries(genicam PRIVATE ${GC_OSAL_LIBRARIES})
elseif(WIN32)
	target_sources(genicam PRIVATE src/osal/osal.c)
	target_compile_definitions(genicam PRIVATE _WIN32_WINNT=0x0600)
	target_link_libraries(genicam PRIVATE ws2_32 iphlpapi)
else()
	target_sources(genicam PRIVATE src/osal/osal.c)
	find_package(Threads REQUIRED)
	set(GC_FIND_THREADS TRUE)
	target_compile_definitions(genicam PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
	target_link_libraries(genicam PRIVATE Threads::Threads m)
endif()

function(gc_warnings target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
	endif()
endfunction()
gc_warnings(genicam)

if(GC_BUILD_DEMOS)
	add_executable(genicam-server demo/server.c)
	target_include_directories(genicam-server PRIVATE src)
	target_link_libraries(genicam-server PRIVATE genicam)
	gc_warnings(genicam-server)

	add_executable(genicam-control demo/control.c)
	target_link_libraries(genicam-control PRIVATE genicam)
	gc_warnings(genicam-control)
endif()

if(BUILD_TESTING)
	add_subdirectory(tests)
endif()

set(gc_install_config "${CMAKE_INSTALL_LIBDIR}/cmake/genicam")
configure_package_config_file(cmake/genicam-config.cmake.in
	"${CMAKE_CURRENT_BINARY_DIR}/genicam-config.cmake"
	INSTALL_DESTINATION "${gc_install_config}")
write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/genicam-config-version.cmake"
	COMPATIBILITY SameMinorVersion)

install(TARGETS genicam EXPORT genicam-targets
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/genicam DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT genicam-targets NAMESPACE genicam::
	DESTINATION "${gc_install_config}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/genicam-config.cmake"
	      "${CMAKE_CURRENT_BINARY_DIR}/genicam-config-version.cmake"
	DESTINATION "${gc_install_config}")
