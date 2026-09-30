# Embeddable ELF loader

option(DISABLE_IOPRP "Disable support for loading IOPRP images" ON)

set(SOURCES
    src/loader.c
    src/ps2logo.c
    src/egsm/egsm.c
    src/egsm/ee_exception_l2.S
    src/eigr/eigr.c
    src/eigr/eigr_asm.S
)

# eIGR is copied over itself and runs while the rest of the loader is overwritten,
# so it must not call memcpy or other library functions
set_source_files_properties(src/eigr/eigr.c PROPERTIES COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns")

if(DISABLE_IOPRP)
  message(NOTICE "ELF Loader IOPRP support will be disabled")
  add_compile_definitions(DISABLE_IOPRP)
endif()

add_executable(loader ${SOURCES})
target_include_directories(loader PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/common/include
    $ENV{PS2SDK}/ee/include
    $ENV{PS2SDK}/common/include
)

target_compile_options(loader PRIVATE
    -Os
    -G0
    -Wall
    -fdata-sections
    -ffunction-sections
)

# Use custom linker script
target_link_options(loader PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/linkfile
)

link_newlib_nano(loader
    fileXio
    iopreboot
    patches
    kernel
    cdvd
    cglue
    pthread
    pthreadglue
)

set_target_properties(loader PROPERTIES
    LINK_FLAGS "-Wl,--gc-sections -s"
    OUTPUT_NAME "loader"
    RUNTIME_OUTPUT_NAME "loader.elf"
)

target_sources(loader PRIVATE ${egsm_source})
target_compile_definitions(loader PRIVATE EGSM_BIN_SIZE)
