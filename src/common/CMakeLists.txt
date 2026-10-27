set(TARGET_NAME linux-wallpaperengine-common)
set(TARGET_SOURCE_FILES
    ${EXTERNAL_DEPS_DIR}/MimeTypes/MimeTypes.cpp
    ${EXTERNAL_DEPS_DIR}/source-parsers/packages/source-parsers-shared/internal/case-insensitive-map.cpp
    ${EXTERNAL_DEPS_DIR}/source-parsers/packages/source-parsers-shared/internal/offset-data-view.cpp
    ${EXTERNAL_DEPS_DIR}/source-parsers/packages/source-parsers-shared/internal/peekable-stream.cpp
    ${EXTERNAL_DEPS_DIR}/source-parsers/packages/vdfparser/keyvalue.cpp
    ${EXTERNAL_DEPS_DIR}/source-parsers/packages/vdfparser/vdf.cpp
    WallpaperEngine/Logging/Log.cpp
    WallpaperEngine/Debugging/CallStack.cpp
    WallpaperEngine/Utils/UUID.cpp
    WallpaperEngine/Maths.cpp
    Steam/FileSystem/FileSystem.cpp)

add_library(${TARGET_NAME}-object OBJECT ${TARGET_SOURCE_FILES})
target_include_directories(${TARGET_NAME}-object PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EXTERNAL_DEPS_DIR}/source-parsers/packages
    ${EXTERNAL_DEPS_DIR}/source-parsers/packages/vdfparser)

# glm (header-only) is used throughout common/ and core/ but was never declared, so the build only
# worked where glm sits on the compiler's default include path. Find it explicitly; PUBLIC so every
# target linking this object library (core included) gets the include dir too.
find_package(glm CONFIG QUIET)
if(TARGET glm::glm)
    target_link_libraries(${TARGET_NAME}-object PUBLIC glm::glm)
else()
    find_path(GLM_INCLUDE_DIR glm/glm.hpp)
    if(NOT GLM_INCLUDE_DIR)
        message(FATAL_ERROR "glm not found (install glm's development package)")
    endif()
    target_include_directories(${TARGET_NAME}-object PUBLIC ${GLM_INCLUDE_DIR})
endif()