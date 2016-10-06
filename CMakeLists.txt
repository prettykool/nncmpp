project (nncmpp C)
cmake_minimum_required (VERSION 2.8.5)

# Moar warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99")
	set (CMAKE_C_FLAGS_DEBUG
		"${CMAKE_C_FLAGS_DEBUG} -Wall -Wextra -Wno-unused-function")
endif ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)

# Version
set (project_VERSION_MAJOR "0")
set (project_VERSION_MINOR "1")
set (project_VERSION_PATCH "0")

set (project_VERSION "${project_VERSION_MAJOR}")
set (project_VERSION "${project_VERSION}.${project_VERSION_MINOR}")
set (project_VERSION "${project_VERSION}.${project_VERSION_PATCH}")

# For custom modules
set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

# Dependencies
find_package (Ncursesw REQUIRED)
find_package (PkgConfig REQUIRED)
find_package (Unistring REQUIRED)
pkg_check_modules (curl REQUIRED libcurl)

set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/liberty/cmake)
include (AddThreads)

find_package (Termo QUIET NO_MODULE)
option (USE_SYSTEM_TERMO
	"Don't compile our own termo library, use the system one" ${Termo_FOUND})

if (USE_SYSTEM_TERMO)
	if (NOT Termo_FOUND)
		message (FATAL_ERROR "System termo library not found")
	endif (NOT Termo_FOUND)
else (USE_SYSTEM_TERMO)
	add_subdirectory (termo EXCLUDE_FROM_ALL)
	# We don't have many good choices when we don't want to install it and want
	# to support older versions of CMake; this is a relatively clean approach
	# (other possibilities: setting a variable in the parent scope, using a
	# cache variable, writing a special config file with build paths in it and
	# including it here, or setting a custom property on the targets).
	get_directory_property (Termo_INCLUDE_DIRS
		DIRECTORY termo INCLUDE_DIRECTORIES)
	set (Termo_LIBRARIES termo-static)
endif (USE_SYSTEM_TERMO)

include_directories (${UNISTRING_INCLUDE_DIRS}
	${NCURSESW_INCLUDE_DIRS} ${Termo_INCLUDE_DIRS} ${curl_INCLUDE_DIRS})
link_directories (${curl_LIBRARY_DIRS})

# Configuration
include (CheckFunctionExists)
set (CMAKE_REQUIRED_LIBRARIES ${NCURSESW_LIBRARIES})
CHECK_FUNCTION_EXISTS ("resizeterm" HAVE_RESIZETERM)

# Generate a configuration file
configure_file (${PROJECT_SOURCE_DIR}/config.h.in
	${PROJECT_BINARY_DIR}/config.h)
include_directories (${PROJECT_SOURCE_DIR} ${PROJECT_BINARY_DIR})

# Build the main executable and link it
add_executable (${PROJECT_NAME} ${PROJECT_NAME}.c)
target_link_libraries (${PROJECT_NAME} ${UNISTRING_LIBRARIES}
	${NCURSESW_LIBRARIES} termo-static ${curl_LIBRARIES})
add_threads (${PROJECT_NAME})

# The files to be installed
include (GNUInstallDirs)
install (TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
install (FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

# CPack
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "MPD client")
set (CPACK_PACKAGE_VENDOR "Premysl Janouch")
set (CPACK_PACKAGE_CONTACT "Přemysl Janouch <p.janouch@gmail.com>")
set (CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set (CPACK_PACKAGE_VERSION_MAJOR ${project_VERSION_MAJOR})
set (CPACK_PACKAGE_VERSION_MINOR ${project_VERSION_MINOR})
set (CPACK_PACKAGE_VERSION_PATCH ${project_VERSION_PATCH})
set (CPACK_GENERATOR "TGZ;ZIP")
set (CPACK_PACKAGE_FILE_NAME
	"${PROJECT_NAME}-${project_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set (CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}-${project_VERSION}")
set (CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set (CPACK_SOURCE_IGNORE_FILES "/\\\\.git;/build;/CMakeLists.txt.user")
set (CPACK_SOURCE_PACKAGE_FILE_NAME "${PROJECT_NAME}-${project_VERSION}")

set (CPACK_SET_DESTDIR TRUE)
include (CPack)
