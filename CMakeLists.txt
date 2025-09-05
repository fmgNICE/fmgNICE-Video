cmake_minimum_required(VERSION 3.28...3.30)
project(fmgnice-video VERSION 1.0.1)

# Options
option(ENABLE_VLC "Build with VLC plugin support" ON)
if(NOT ENABLE_VLC)
  message(STATUS "VLC plugin support disabled")
  return()
endif()

# Set OBS source directory
set(OBS_SOURCE_DIR "C:/Users/matt/source/repos/obs-studio" CACHE PATH "OBS Studio source directory")

# Create the plugin module
add_library(fmgnice-video MODULE)

# Add source files
target_sources(fmgnice-video PRIVATE 
  src/plugin-main.c
  src/fmgnice-video-source.c
  src/ffmpeg-decoder.c
  src/ffmpeg-decoder.h
  src/simd-convert.c
  src/simd-convert.h
  src/gpu-zero-copy.c
  src/gpu-zero-copy.h
  src/lockfree-ringbuffer.c
  src/lockfree-ringbuffer.h
  src/frame-cache.c
  src/frame-cache.h
  src/aligned-memory.h
  src/cpu-affinity.h
)

# Platform-specific compile options
if(MSVC)
  # Base warning level
  target_compile_options(fmgnice-video PRIVATE /W3)
  
  # We use Windows atomics instead of C11 atomics for MSVC
  
  # Optimization flags for Release builds
  target_compile_options(fmgnice-video PRIVATE
    $<$<CONFIG:Release>:/O2>      # Maximum optimization
    $<$<CONFIG:Release>:/Ob2>     # Inline function expansion
    $<$<CONFIG:Release>:/Oi>      # Enable intrinsic functions
    $<$<CONFIG:Release>:/Ot>      # Favor fast code
    $<$<CONFIG:Release>:/Oy>      # Omit frame pointers
    $<$<CONFIG:Release>:/GL>      # Whole program optimization
    $<$<CONFIG:Release>:/GF>      # String pooling
    $<$<CONFIG:Release>:/Gw>      # Global data optimization
    $<$<CONFIG:Release>:/fp:fast> # Fast floating point
    $<$<CONFIG:Release>:/arch:AVX2> # Enable AVX2 instructions
  )
  
  # Linker flags for Release builds
  target_link_options(fmgnice-video PRIVATE
    $<$<CONFIG:Release>:/LTCG>    # Link-time code generation
    $<$<CONFIG:Release>:/OPT:REF> # Remove unreferenced code
    $<$<CONFIG:Release>:/OPT:ICF> # Remove identical folding
  )
  
  # Enable parallel compilation
  target_compile_options(fmgnice-video PRIVATE /MP)
  
  # Define optimization macros
  target_compile_definitions(fmgnice-video PRIVATE
    $<$<CONFIG:Release>:NDEBUG>
    $<$<CONFIG:Release>:_RELEASE>
    $<$<CONFIG:Release>:USE_SSE4_2>
    $<$<CONFIG:Release>:USE_AVX2>
  )
endif()

# Include directories
target_include_directories(fmgnice-video PRIVATE
  "${OBS_SOURCE_DIR}/libobs"
  "${OBS_SOURCE_DIR}/build_libs/libobs"  # For generated headers
  "${OBS_SOURCE_DIR}/build_libs/config"   # For obsconfig.h
  "${OBS_SOURCE_DIR}/deps/w32-pthreads"   # For pthread.h on Windows
  "${OBS_SOURCE_DIR}/shared/media-playback"  # For media-playback API
  "${OBS_SOURCE_DIR}/.deps/obs-deps-2025-07-11-x64/include"  # For FFmpeg headers
)

# Find VLC
find_path(
  LibVLC_INCLUDE_DIR
  NAMES vlc/libvlc.h
  HINTS ${VLC_PATH}/include
  PATHS 
    "C:/Users/matt/source/repos/obs-studio/.deps/obs-deps-2025-07-11-x64/include"
    "C:/Users/matt/source/repos/vlc/include"
    "C:/Program Files/VideoLAN/VLC/sdk/include"
    "C:/Program Files (x86)/VideoLAN/VLC/sdk/include"
    /usr/include 
    /usr/local/include
  DOC "LibVLC include directory"
)

if(LibVLC_INCLUDE_DIR)
  target_include_directories(fmgnice-video PRIVATE "${LibVLC_INCLUDE_DIR}")
  message(STATUS "Found VLC headers at: ${LibVLC_INCLUDE_DIR}")
else()
  message(WARNING "VLC headers not found. Plugin will use dynamic loading.")
endif()

# Link libraries
if(WIN32)
  # Find OBS libraries
  find_library(OBS_LIB 
    NAMES obs libobs
    PATHS 
      "${OBS_SOURCE_DIR}/build_libs/libobs/Release"
      "${OBS_SOURCE_DIR}/build_libs/libobs/Debug"
      "${OBS_SOURCE_DIR}/build_libs/x64/Release"
      "${OBS_SOURCE_DIR}/build_libs/x64/Debug"
    )
  
  if(OBS_LIB)
    target_link_libraries(fmgnice-video PRIVATE ${OBS_LIB})
    message(STATUS "Found OBS library: ${OBS_LIB}")
  else()
    message(WARNING "OBS library not found. You may need to build OBS first or adjust paths.")
  endif()
  
  # Find and link w32-pthreads
  find_library(W32_PTHREADS_LIB
    NAMES w32-pthreads
    PATHS
      "${OBS_SOURCE_DIR}/build_libs/deps/w32-pthreads/Release"
      "${OBS_SOURCE_DIR}/build_libs/deps/w32-pthreads/Debug"
  )
  
  if(W32_PTHREADS_LIB)
    target_link_libraries(fmgnice-video PRIVATE ${W32_PTHREADS_LIB})
    message(STATUS "Found w32-pthreads: ${W32_PTHREADS_LIB}")
  endif()
  
  # Find FFmpeg libraries for media-playback
  set(FFMPEG_LIB_DIR "${OBS_SOURCE_DIR}/.deps/obs-deps-2025-07-11-x64/lib")
  find_library(AVFORMAT_LIB avformat PATHS ${FFMPEG_LIB_DIR})
  find_library(AVCODEC_LIB avcodec PATHS ${FFMPEG_LIB_DIR})
  find_library(AVUTIL_LIB avutil PATHS ${FFMPEG_LIB_DIR})
  find_library(SWSCALE_LIB swscale PATHS ${FFMPEG_LIB_DIR})
  find_library(SWRESAMPLE_LIB swresample PATHS ${FFMPEG_LIB_DIR})
  find_library(AVDEVICE_LIB avdevice PATHS ${FFMPEG_LIB_DIR})
  
  if(AVFORMAT_LIB AND AVCODEC_LIB AND AVUTIL_LIB AND SWSCALE_LIB AND SWRESAMPLE_LIB AND AVDEVICE_LIB)
    target_link_libraries(fmgnice-video PRIVATE 
      ${AVFORMAT_LIB} ${AVCODEC_LIB} ${AVUTIL_LIB} ${SWSCALE_LIB} ${SWRESAMPLE_LIB} ${AVDEVICE_LIB})
    message(STATUS "Found FFmpeg libraries")
  else()
    message(WARNING "FFmpeg libraries not found. Timeline source may not work.")
  endif()
  
  # Windows libraries
  target_link_libraries(fmgnice-video PRIVATE
    kernel32
    user32
    gdi32
    winspool
    comdlg32
    advapi32
    shell32
    ole32
    oleaut32
    uuid
    d3d11
    dxgi
    dxguid
  )
  
  # Define Windows version
  target_compile_definitions(fmgnice-video PRIVATE
    _WIN32_WINNT=0x0602
    WINVER=0x0602
    _CRT_SECURE_NO_WARNINGS  # Disable deprecation warnings
  )
endif()

# Windows-specific files
if(WIN32)
  # Add module definition file for proper exports
  target_sources(fmgnice-video PRIVATE fmgnice-video.def)
  
  # Add resource file if it exists
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fmgnice-video.rc")
    target_sources(fmgnice-video PRIVATE fmgnice-video.rc)
  endif()
endif()

# Set properties
set_target_properties(fmgnice-video PROPERTIES 
  PREFIX ""
  OUTPUT_NAME "fmgnice-video"
  INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE  # Enable LTO for Release
)

# Output directory
set_target_properties(fmgnice-video PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/Debug"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/Release"
  LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/Debug"
  LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/Release"
)

message(STATUS "fmgNICE Video Source Plugin configuration complete")
message(STATUS "  OBS Source: ${OBS_SOURCE_DIR}")
message(STATUS "  VLC Include: ${LibVLC_INCLUDE_DIR}")