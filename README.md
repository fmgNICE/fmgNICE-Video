# fmgNICE Video Source Plugin for OBS Studio

An OBS Studio plugin that provides high-performance video playback with precise frame synchronization, hardware acceleration via GPU zero-copy, SIMD-optimized color conversion, lock-free ring buffering, intelligent frame caching, and FFmpeg-based decoding for smooth, synchronized streaming.

## Features

### Core Capabilities
- **Synchronized Timeline Playback**: Precise frame-accurate synchronization across multiple video sources
- **Playlist Support**: Sequential playback of multiple video files with seamless transitions
- **Loop Playback**: Automatic playlist looping for continuous streaming

### Performance Optimizations
- **Hardware Acceleration**: GPU zero-copy rendering with Direct3D 11 support
- **SIMD Processing**: SSE4.2 and AVX2 optimized color conversion (YUV to BGRA/NV12)
- **Lock-Free Ring Buffer**: High-performance thread-safe frame buffering
- **Intelligent Frame Caching**: Memory-efficient frame cache with LRU eviction
- **CPU Affinity Management**: Optimized thread scheduling for decoder threads
- **Aligned Memory Allocation**: SIMD-aligned memory management for optimal performance

### Advanced Features
- **Multiple Hardware Decoders**: Support for D3D11VA, DXVA2, CUDA, and Intel QuickSync
- **Configurable Performance Modes**: Quality, Balanced, and Performance presets
- **Frame Drop Support**: Adaptive frame dropping for maintaining sync
- **Seek Modes**: Accurate or fast seeking options
- **Audio Buffer Management**: Configurable audio buffering for smooth playback
- **Deferred Shutdown**: Smart resource management for rapid scene switching

## System Requirements

- **Operating System**: Windows 10/11 (64-bit)
- **OBS Studio**: Version 28.0 or newer
- **Visual Studio**: 2022 Community or higher (for building)
- **CMake**: Version 3.28 or newer
- **CPU**: SSE4.2 support required, AVX2 recommended
- **GPU**: Direct3D 11 compatible graphics card

## Prerequisites

Before building, ensure you have:
1. OBS Studio source code (required for headers and libraries)
2. OBS dependencies package (obs-deps-2025-07-11-x64 or newer)
3. FFmpeg libraries (included in OBS dependencies)
4. Visual Studio 2022 with C++ development tools
5. CMake 3.28+

## Building from Source

### Quick Build
```batch
# Clone the repository
git clone https://github.com/fmgNICE/fmgNICE-Video.git
cd fmgnice-video

# Build the plugin
build.bat
```

### Manual Build
1. Configure the project with CMake:
```batch
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DOBS_SOURCE_DIR="C:/path/to/obs-studio"
```

2. Build the plugin:
```batch
cmake --build . --config Release
```

The plugin DLL will be created at `build/Release/fmgnice-video.dll`

## Installation

### Automatic Installation
```batch
# Run the installation script (requires administrator privileges)
install.bat
```

### Manual Installation
1. Copy `fmgnice-video.dll` to:
   ```
   C:\ProgramData\obs-studio\plugins\fmgnice-video\bin\64bit\
   ```

2. Copy data files (if any) to:
   ```
   C:\ProgramData\obs-studio\plugins\fmgnice-video\data\
   ```

3. Restart OBS Studio

## Usage

1. In OBS Studio, click the "+" button in the Sources panel
2. Select "fmgNICE Video Source" from the list
3. Configure your video playlist and settings
4. Adjust performance options based on your hardware

## Configuration Options

### Playlist Settings
- **Playlist**: Add multiple video files for sequential playback
- **Loop Playlist**: Enable continuous looping of the playlist

### Hardware Decoding
- **Use Hardware Decoding**: Enable GPU-accelerated video decoding
- **Hardware Decoder**: Select specific decoder (Auto, D3D11VA, DXVA2, CUDA, QuickSync)

### Buffer Settings
- **Frame Buffer Size**: Number of frames to buffer (default: 30)
- **Pre-buffer Time**: Milliseconds to pre-buffer before playback (default: 500ms)
- **Audio Buffer**: Audio buffer size in milliseconds (default: 200ms)
- **Cache Size**: Frame cache size in MB (default: 128MB)

### Synchronization
- **Sync Mode**: Global, Local, or Disabled synchronization
- **Sync Offset**: Timing offset in milliseconds
- **Allow Frame Drop**: Enable adaptive frame dropping

### Performance Modes
- **Quality**: Maximum quality, all optimizations disabled
- **Balanced**: Good quality with moderate optimizations
- **Performance**: Maximum performance, aggressive optimizations

### Output Format
- **BGRA**: Maximum compatibility (default)
- **NV12**: Better performance with compatible GPUs

## Performance Tips

1. **Enable Hardware Decoding**: Use D3D11VA for best Windows performance
2. **Adjust Buffer Sizes**: Larger buffers improve stability but increase memory usage
3. **Use Performance Mode**: For multiple simultaneous sources or lower-end hardware
4. **Enable Frame Drop**: Helps maintain sync during high CPU load
5. **Use NV12 Output**: Reduces memory bandwidth on compatible systems

## Troubleshooting

### Plugin Not Loading
- Verify OBS Studio version is 28.0 or newer
- Check that Visual C++ Redistributables are installed
- Ensure the plugin is in the correct directory

### Poor Performance
- Enable hardware decoding
- Reduce buffer sizes if memory is limited
- Switch to Performance mode
- Enable frame dropping
- Check CPU/GPU usage in Task Manager

### Sync Issues
- Adjust sync offset for your specific setup
- Enable frame dropping for better sync maintenance
- Check that all sources use the same sync mode

### Decoder Failures
- Try different hardware decoder options
- Disable hardware decoding as a fallback
- Verify video codec compatibility

## Uninstallation

Run the uninstall script:
```batch
uninstall.bat
```

Or manually remove:
```
C:\ProgramData\obs-studio\plugins\fmgnice-video\
```

## Development

### Project Structure
```
fmgnice-video/
├── src/
│   ├── plugin-main.c           # Plugin entry point
│   ├── fmgnice-video-source.c  # Main source implementation
│   ├── ffmpeg-decoder.c        # Custom FFmpeg decoder
│   ├── gpu-zero-copy.c         # D3D11 zero-copy rendering
│   ├── simd-convert.c          # SIMD color conversion
│   ├── lockfree-ringbuffer.c   # Lock-free frame buffer
│   ├── frame-cache.c           # Intelligent frame caching
│   └── *.h                     # Header files
├── data/                        # Plugin data files
├── cmake/                       # CMake modules
├── build.bat                    # Build script
├── install.bat                  # Installation script
├── uninstall.bat               # Uninstallation script
└── CMakeLists.txt              # CMake configuration
```

### Key Technologies
- **FFmpeg**: Video decoding and format handling
- **Direct3D 11**: Hardware acceleration and zero-copy rendering
- **SIMD Intrinsics**: SSE4.2/AVX2 for color conversion
- **Lock-Free Algorithms**: High-performance concurrent data structures
- **Windows Atomics**: Thread synchronization primitives

## Contributing

Contributions are welcome! Please ensure:
1. Code follows the existing style conventions
2. Performance optimizations are properly tested
3. Hardware-specific code has appropriate fallbacks
4. Documentation is updated for new features

## License

This project is licensed under the GNU General Public License v2.0 (GPL-2.0).

Copyright (C) 2025 fmgNICE Team

This software uses code from:
- OBS Studio (GPL-2.0): https://obsproject.com/
- FFmpeg (LGPL-2.1+): https://ffmpeg.org/

See [LICENSE](LICENSE) file for full license text.

## Acknowledgments

- OBS Studio team for the excellent plugin framework
- FFmpeg developers for the powerful multimedia libraries
- Contributors and testers who helped improve this plugin

## Support

For issues, feature requests, or questions:
- Create an issue on GitHub
- Check existing issues for solutions
- Include OBS Studio logs when reporting problems
