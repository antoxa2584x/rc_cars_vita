# Delete every rc_cars_vita*.vpk in DIR except KEEP -- run after the package is
# built, so the folder holds exactly the build that was just made. Each vpk is
# ~290 MB and they differ only by version, so keeping them costs gigabytes and
# invites installing a stale one. An older build is `git checkout` + make away.
#
#   cmake -DDIR=<build dir> -DKEEP=<file name> -P prune_vpks.cmake
if(NOT DIR OR NOT KEEP)
  message(FATAL_ERROR "prune_vpks.cmake needs -DDIR and -DKEEP")
endif()
if(NOT EXISTS "${DIR}/${KEEP}")
  # Never prune when the new package is not there: that would leave nothing.
  message(WARNING "prune_vpks: ${DIR}/${KEEP} missing, not pruning")
  return()
endif()
file(GLOB _old "${DIR}/rc_cars_vita*.vpk")
foreach(_f IN LISTS _old)
  get_filename_component(_n "${_f}" NAME)
  if(NOT _n STREQUAL KEEP)
    file(REMOVE "${_f}")
    message(STATUS "prune_vpks: removed previous build ${_n}")
  endif()
endforeach()
