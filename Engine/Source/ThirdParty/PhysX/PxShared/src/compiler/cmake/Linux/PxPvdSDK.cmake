#
# Build PxPvdSDK
#

SET(GW_DEPS_ROOT $ENV{GW_DEPS_ROOT})

SET(PXSHARED_SOURCE_DIR ${PROJECT_SOURCE_DIR}/../../../../src)

SET(LL_SOURCE_DIR ${PXSHARED_SOURCE_DIR}/pvd)

SET(PXPVDSDK_LIBTYPE STATIC)

# Use generator expressions to set config specific preprocessor definitions
SET(PXPVDSDK_COMPILE_DEFS 
	# Common to all configurations
	${PXSHARED_Linux_COMPILE_DEFS}
)

if(${CMAKE_BUILD_TYPE_LOWERCASE} STREQUAL "debug")
	LIST(APPEND PXPVDSDK_COMPILE_DEFS 
		${PXSHARED_LINUX_DEBUG_COMPILE_DEFS}
	)
elseif(${CMAKE_BUILD_TYPE_LOWERCASE} STREQUAL "checked")
	LIST(APPEND PXPVDSDK_COMPILE_DEFS 
		${PXSHARED_LINUX_CHECKED_COMPILE_DEFS}
	)
elseif(${CMAKE_BUILD_TYPE_LOWERCASE} STREQUAL "profile")
	LIST(APPEND PXPVDSDK_COMPILE_DEFS 
		${PXSHARED_LINUX_PROFILE_COMPILE_DEFS}
	)
elseif(${CMAKE_BUILD_TYPE_LOWERCASE} STREQUAL release)
	LIST(APPEND PXPVDSDK_COMPILE_DEFS 
		${PXSHARED_LINUX_RELEASE_COMPILE_DEFS}
	)
else(${CMAKE_BUILD_TYPE_LOWERCASE} STREQUAL "debug")
	MESSAGE(FATAL_ERROR "Unknown configuration ${CMAKE_BUILD_TYPE}")
endif(${CMAKE_BUILD_TYPE_LOWERCASE} STREQUAL "debug")

# include PxPvdSDK common
INCLUDE(../common/PxPvdSDK.cmake)

# Add linked libraries
TARGET_LINK_LIBRARIES(PxPvdSDK PRIVATE PxFoundation)

# enable -fPIC so we can link static libs with the editor
SET_TARGET_PROPERTIES(PxPvdSDK PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
