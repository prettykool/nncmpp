cmake_minimum_required (VERSION 3.0)
project (nncmpp VERSION 1.1.0 LANGUAGES C)

# Moar warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUCC)
	set (wdisabled "-Wno-unused-function")
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -Wall -Wextra ${wdisabled}")
endif ()

# For custom modules
set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/liberty/cmake)

# Dependencies
find_package (Ncursesw REQUIRED)
find_package (PkgConfig REQUIRED)
find_package (Unistring REQUIRED)
pkg_check_modules (curl REQUIRED libcurl)

include (AddThreads)

find_package (Termo QUIET NO_MODULE)
option (USE_SYSTEM_TERMO
	"Don't compile our own termo library, use the system one" ${Termo_FOUND})
if (USE_SYSTEM_TERMO)
	if (NOT Termo_FOUND)
		message (FATAL_ERROR "System termo library not found")
	endif (NOT Termo_FOUND)
else ()
	add_subdirectory (termo EXCLUDE_FROM_ALL)
	# We don't have many good choices when we don't want to install it and want
	# to support older versions of CMake; this is a relatively clean approach
	# (other possibilities: setting a variable in the parent scope, using a
	# cache variable, writing a special config file with build paths in it and
	# including it here, or setting a custom property on the targets).
	get_directory_property (Termo_INCLUDE_DIRS
		DIRECTORY termo INCLUDE_DIRECTORIES)
	set (Termo_LIBRARIES termo-static)
endif ()

pkg_check_modules (fftw fftw3 fftw3f)
option (WITH_FFTW "Use FFTW to enable spectrum visualisation" ${fftw_FOUND})
if (WITH_FFTW)
	if (NOT fftw_FOUND)
		message (FATAL_ERROR "FFTW not found")
	endif()
endif ()

include_directories (${Unistring_INCLUDE_DIRS}
	${Ncursesw_INCLUDE_DIRS} ${Termo_INCLUDE_DIRS} ${curl_INCLUDE_DIRS}
	${fftw_INCLUDE_DIRS})
link_directories (${curl_LIBRARY_DIRS} ${fftw_LIBRARY_DIRS})

# Configuration
include (CheckFunctionExists)
set (CMAKE_REQUIRED_LIBRARIES ${Ncursesw_LIBRARIES})
CHECK_FUNCTION_EXISTS ("resizeterm" HAVE_RESIZETERM)

if ("${CMAKE_SYSTEM_NAME}" MATCHES "BSD")
	# Need this for SIGWINCH in FreeBSD and OpenBSD respectively;
	# our POSIX version macros make it undefined
	add_definitions (-D__BSD_VISIBLE=1 -D_BSD_SOURCE=1)
endif ()

# -lm may or may not be a part of libc
foreach (extra m)
	find_library (extra_lib_${extra} ${extra})
	if (extra_lib_${extra})
		list (APPEND extra_libraries ${extra_lib_${extra}})
	endif ()
endforeach ()

# Generate a configuration file
configure_file (${PROJECT_SOURCE_DIR}/config.h.in
	${PROJECT_BINARY_DIR}/config.h)
include_directories (${PROJECT_SOURCE_DIR} ${PROJECT_BINARY_DIR})

# Build the main executable and link it
add_executable (${PROJECT_NAME} ${PROJECT_NAME}.c)
target_link_libraries (${PROJECT_NAME} ${Unistring_LIBRARIES}
	${Ncursesw_LIBRARIES} termo-static ${curl_LIBRARIES}
	${fftw_LIBRARIES} ${extra_libraries})
add_threads (${PROJECT_NAME})

# Installation
include (GNUInstallDirs)
install (TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
install (FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
install (DIRECTORY contrib DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME})

# Generate documentation from text markup
find_program (ASCIIDOCTOR_EXECUTABLE asciidoctor)
if (NOT ASCIIDOCTOR_EXECUTABLE)
	message (FATAL_ERROR "asciidoctor not found")
endif ()

foreach (page ${PROJECT_NAME})
	set (page_output "${PROJECT_BINARY_DIR}/${page}.1")
	list (APPEND project_MAN_PAGES "${page_output}")
	add_custom_command (OUTPUT ${page_output}
		COMMAND ${ASCIIDOCTOR_EXECUTABLE} -b manpage
			-a release-version=${PROJECT_VERSION}
			"${PROJECT_SOURCE_DIR}/${page}.adoc"
			-o "${page_output}"
		DEPENDS ${page}.adoc
		COMMENT "Generating man page for ${page}" VERBATIM)
endforeach ()

add_custom_target (docs ALL DEPENDS ${project_MAN_PAGES})

foreach (page ${project_MAN_PAGES})
	string (REGEX MATCH "\\.([0-9])$" manpage_suffix "${page}")
	install (FILES "${page}"
		DESTINATION "${CMAKE_INSTALL_MANDIR}/man${CMAKE_MATCH_1}")
endforeach ()

# CPack
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "MPD client")
set (CPACK_PACKAGE_VENDOR "Premysl Eric Janouch")
set (CPACK_PACKAGE_CONTACT "Přemysl Eric Janouch <p@janouch.name>")
set (CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set (CPACK_GENERATOR "TGZ;ZIP")
set (CPACK_PACKAGE_FILE_NAME
	"${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set (CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}-${PROJECT_VERSION}")
set (CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set (CPACK_SOURCE_IGNORE_FILES "/\\\\.git;/build;/CMakeLists.txt.user")
set (CPACK_SOURCE_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}")

set (CPACK_SET_DESTDIR TRUE)
include (CPack)
