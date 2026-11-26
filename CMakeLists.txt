cmake_minimum_required(VERSION 3.16)
project(genicam VERSION 0.1.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(GC_BUILD_HOST "Build the camera controller SDK" ON)
option(GC_BUILD_CAMERA "Build the camera device SDK" ON)
option(GC_BUILD_DEMOS "Build the demos for the enabled roles" ON)
set(GC_OSAL_SOURCE "" CACHE FILEPATH "Replacement for the built-in OSAL source")
set(GC_OSAL_LIBRARIES "" CACHE STRING "Libraries for the replacement OSAL source")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(gc_install_config "${CMAKE_INSTALL_LIBDIR}/cmake/genicam")
set(gc_objects "")
set(gc_dependencies "")
set(gc_platform_definitions "")
set(GC_FIND_THREADS FALSE)
if(GC_OSAL_SOURCE)
	set(gc_osal_source "${GC_OSAL_SOURCE}")
	set(gc_platform_libraries ${GC_OSAL_LIBRARIES})
elseif(WIN32)
	set(gc_osal_source src/osal/osal.c)
	set(gc_platform_definitions _WIN32_WINNT=0x0600)
	set(gc_platform_libraries ws2_32 iphlpapi)
else()
	find_package(Threads REQUIRED)
	set(GC_FIND_THREADS TRUE)
	set(gc_osal_source src/osal/osal.c)
	set(gc_platform_definitions _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
	set(gc_platform_libraries Threads::Threads)
endif()

function(gc_warnings target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
	endif()
endfunction()

function(gc_install_target target component)
	install(TARGETS ${target} EXPORT genicam-${component}-targets
		ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
	install(EXPORT genicam-${component}-targets NAMESPACE genicam::
		DESTINATION "${gc_install_config}")
endfunction()

function(gc_add_component component)
	cmake_parse_arguments(PART "" "" "SOURCES;DEPENDENCIES;PUBLIC" ${ARGN})
	add_library(genicam_${component}_objects OBJECT ${PART_SOURCES})
	target_include_directories(genicam_${component}_objects PRIVATE include src)
	target_compile_definitions(genicam_${component}_objects PRIVATE ${gc_platform_definitions})
	target_link_libraries(genicam_${component}_objects PRIVATE ${PART_DEPENDENCIES})
	gc_warnings(genicam_${component}_objects)

	set(objects "$<TARGET_OBJECTS:genicam_${component}_objects>")
	add_library(genicam_${component} STATIC ${objects})
	add_library(genicam::${component} ALIAS genicam_${component})
	set_target_properties(genicam_${component} PROPERTIES EXPORT_NAME ${component})
	target_include_directories(genicam_${component} PUBLIC
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
		"$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
	target_link_libraries(genicam_${component}
		PUBLIC ${PART_PUBLIC} PRIVATE ${PART_DEPENDENCIES})
	gc_install_target(genicam_${component} ${component})

	set(gc_objects ${gc_objects} "${objects}" PARENT_SCOPE)
	set(gc_dependencies ${gc_dependencies} ${PART_DEPENDENCIES} PARENT_SCOPE)
endfunction()

gc_add_component(common
	SOURCES
		src/common/frame.c
		src/common/frame_queue.c
		src/common/pfnc.c
		src/common/status.c
		src/gendc/container.c
		src/transport/gige/wire/protocol.c
		"${gc_osal_source}"
	DEPENDENCIES ${gc_platform_libraries}
)

if(GC_BUILD_HOST)
	find_package(EXPAT REQUIRED)
	find_package(ZLIB REQUIRED)
	set(gc_host_libraries EXPAT::EXPAT ZLIB::ZLIB)
	if(NOT WIN32 AND NOT GC_OSAL_SOURCE)
		list(APPEND gc_host_libraries m)
	endif()
	gc_add_component(host
		SOURCES
			src/host/device.c
			src/host/queue.c
			src/host/genapi/feature.c
			src/host/genapi/formula.c
			src/host/genapi/xml.c
			src/host/genapi/zip.c
			src/transport/gige/host/client.c
			src/transport/gige/host/stream.c
		PUBLIC genicam_common
		DEPENDENCIES ${gc_host_libraries}
	)
endif()

if(GC_BUILD_CAMERA)
	gc_add_component(camera
		SOURCES
			src/camera/camera.c
			src/transport/gige/camera/device.c
			src/transport/gige/camera/server.c
		PUBLIC genicam_common
	)
endif()

# Keep the original archive and CMake target for existing applications.
add_library(genicam STATIC ${gc_objects})
add_library(genicam::genicam ALIAS genicam)
target_include_directories(genicam PUBLIC
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
	"$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(genicam PRIVATE ${gc_dependencies})
gc_install_target(genicam compat)

if(GC_BUILD_DEMOS AND GC_BUILD_CAMERA)
	set(gc_demo_xml "${CMAKE_CURRENT_SOURCE_DIR}/demo/camera/camera.xml")
	add_executable(genicam-server demo/camera/server.c)
	target_compile_definitions(genicam-server PRIVATE GC_DEMO_XML_FILE="${gc_demo_xml}")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${gc_demo_xml}")
	file(GENERATE OUTPUT "$<TARGET_FILE_DIR:genicam-server>/camera.xml"
		INPUT "${gc_demo_xml}")
	target_include_directories(genicam-server PRIVATE src)
	target_link_libraries(genicam-server PRIVATE genicam::camera)
	gc_warnings(genicam-server)
endif()

if(GC_BUILD_DEMOS AND GC_BUILD_HOST)
	add_executable(genicam-control demo/host/control.c)
	target_link_libraries(genicam-control PRIVATE genicam::host)
	gc_warnings(genicam-control)
endif()

if(BUILD_TESTING)
	add_subdirectory(tests)
endif()

configure_package_config_file(cmake/genicam-config.cmake.in
	"${CMAKE_CURRENT_BINARY_DIR}/genicam-config.cmake"
	INSTALL_DESTINATION "${gc_install_config}")
write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/genicam-config-version.cmake"
	COMPATIBILITY SameMinorVersion)

install(DIRECTORY include/genicam DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/genicam-config.cmake"
	      "${CMAKE_CURRENT_BINARY_DIR}/genicam-config-version.cmake"
	DESTINATION "${gc_install_config}")
