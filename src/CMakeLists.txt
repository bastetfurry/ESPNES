# Every source in src/, including the generated ROM index and the ROMs' .incbin (src/generated)
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.S)
idf_component_register(SRCS ${app_sources})
# the emulator wants speed; the IDF is built for size (sdkconfig.defaults)
# (the IDF makes some warnings errors; these two have false alarms here: the PPU's maps, the
# file manager's sizes)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-error=array-bounds -Wno-error=format-truncation)
