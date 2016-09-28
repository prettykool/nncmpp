# Public Domain

find_package (PkgConfig REQUIRED)
pkg_check_modules (NCURSESW QUIET ncursesw)

# OpenBSD doesn't provide a pkg-config file
set (required_vars NCURSESW_LIBRARIES)
if (NOT NCURSESW_FOUND)
	find_library (NCURSESW_LIBRARIES NAMES ncursesw)
	find_path (NCURSESW_INCLUDE_DIRS ncurses.h)
	list (APPEND required_vars NCURSESW_INCLUDE_DIRS)
endif (NOT NCURSESW_FOUND)

include (FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS (NCURSESW DEFAULT_MSG ${required_vars})

mark_as_advanced (NCURSESW_LIBRARIES NCURSESW_INCLUDE_DIRS)
