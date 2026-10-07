cmake_minimum_required(VERSION 3.20)
project(podcast_preview C CXX)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE PATH "" FORCE)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(APP "${REPO}/main")
add_subdirectory("${REPO}/managed_components/lvgl__lvgl" lvgl)
add_executable(preview preview.c ${APP}/podcast_ui.c ${APP}/podcast_covers.c ${APP}/podcast_cover_dynamic.c ${APP}/podcast_cover_wire.c ${APP}/app_cjk_18.c ${APP}/app_ui_16.c ${APP}/podcast_fonts.c ${APP}/podcast_font_codec.c)
target_include_directories(preview PRIVATE ${APP})
target_link_libraries(preview lvgl m)
# This executable performs runtime assertions even in a release host build.
target_compile_options(preview PRIVATE -UNDEBUG)

add_executable(setup_preview setup_preview.c ${APP}/podcast_setup_screen.c ${APP}/app_cjk_18.c ${APP}/app_ui_16.c ${APP}/podcast_font_codec.c)
target_include_directories(setup_preview PRIVATE ${APP})
target_link_libraries(setup_preview lvgl m)
target_compile_options(setup_preview PRIVATE -UNDEBUG)

# Pure ruler mapping is tested independently of the drawing engine.
enable_testing()
add_executable(test_podcast_ruler ${REPO}/tests/test_podcast_ruler.c)
target_include_directories(test_podcast_ruler PRIVATE ${APP})
target_compile_options(test_podcast_ruler PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME podcast_ruler COMMAND test_podcast_ruler)

add_executable(test_podcast_font_codec ${REPO}/tests/test_podcast_font_codec.c ${APP}/podcast_font_codec.c)
target_include_directories(test_podcast_font_codec PRIVATE ${APP})
target_link_libraries(test_podcast_font_codec lvgl)
target_compile_options(test_podcast_font_codec PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME podcast_font_codec COMMAND test_podcast_font_codec)

# An independent uncompressed converter artifact is supplied for pixel-for-
# pixel acceptance, never linked into device firmware or checked into assets.
set(PODCAST_ORIGINAL_FONT_SOURCE "" CACHE FILEPATH "Original uncompressed app_cjk_18.c for lossless acceptance")
if(PODCAST_ORIGINAL_FONT_SOURCE)
    set_source_files_properties(${PODCAST_ORIGINAL_FONT_SOURCE} PROPERTIES COMPILE_DEFINITIONS "app_cjk_18=app_cjk_18_original")
    add_executable(test_podcast_font_lossless ${REPO}/tests/test_podcast_font_lossless.c
        ${APP}/app_cjk_18.c ${APP}/podcast_font_codec.c ${PODCAST_ORIGINAL_FONT_SOURCE})
    target_include_directories(test_podcast_font_lossless PRIVATE ${APP})
    target_link_libraries(test_podcast_font_lossless lvgl)
    target_compile_options(test_podcast_font_lossless PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    add_test(NAME podcast_font_lossless COMMAND test_podcast_font_lossless)
endif()
