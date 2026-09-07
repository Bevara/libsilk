cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBSILK_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_silk.c
)

SET(LIBSILK_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libSKP_SILK_SDK.a
)

SET(LIBSILK_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libsilk
        "${LIBSILK_SRC}"
        "${LIBSILK_LIB}"
        []
        ""
        "${LIBSILK_INC}"
        ""
        "1")
