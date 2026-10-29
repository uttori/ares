# Compile the pinned encoder/decoder for the runtime's own architecture and OS
# deployment target. No package-manager dylib or worker threads are required.
file(GLOB zstd_sources CONFIGURE_DEPENDS lib/common/*.c lib/compress/*.c lib/decompress/*.c)
add_library(uttori-zstd STATIC ${zstd_sources})
add_library(zstd::libzstd_static ALIAS uttori-zstd)
target_include_directories(uttori-zstd PUBLIC lib)
target_compile_definitions(uttori-zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0)
set_target_properties(uttori-zstd PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER thirdparty PREFIX "")
