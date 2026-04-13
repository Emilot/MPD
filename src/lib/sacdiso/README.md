# SACD ISO Decoder Plugin for MPD

This plugin enables native playback of SACD (Super Audio CD) ISO images in Music Player Daemon.

## Features

- **Native DSD playback**: Outputs DSD64 (2.8224 MHz) directly to compatible hardware
- **Stereo and Multichannel support**: Both 2-channel and multichannel areas
- **DST decoding**: Full DST (lossless compression) support via FFmpeg
- **Container scanning**: Each track appears as a separate song in your library
- **Metadata extraction**: Album title, artist, track names from disc TOC
- **Seeking**: Full seek support within tracks

## Dependencies

- **FFmpeg** (libavcodec, libavutil) - Required for DST decoding
  - Most SACDs (~90%) use DST compression
  - Without FFmpeg, only raw DSD content will play

## Configuration

Add to your `mpd.conf`:

```conf
decoder {
    plugin "sacdiso"
    enabled "yes"
    
    # Optional settings:
    edited_master "false"      # Use edited master track boundaries
    playable_area "both"       # "stereo", "multichannel", or "both"
    lsbitfirst "false"         # Bit reversal for LSB-first DACs
}
```

### Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `edited_master` | `false` | Use edited master mode for track boundaries |
| `playable_area` | `both` | Which area(s) to expose: `stereo`, `multichannel`, or `both` |
| `lsbitfirst` | `false` | Reverse bit order for DACs expecting LSB-first DSD |

## Building

Enable the plugin in meson:

```bash
meson setup build -Dsacdiso=true -Ddsd=true -Dffmpeg=enabled
meson compile -C build
```

### Build Requirements

- MPD source code
- FFmpeg development libraries (libavcodec-dev, libavutil-dev)
- C++23 compiler (GCC 12+ or Clang 14+)

## Usage

1. Place your SACD ISO files in your music library
2. Update your database: `mpc update`
3. The ISO will appear as a container with individual tracks

### Track Naming

Tracks are named using the following format:
- Stereo: `2C_AUDIO__TRACK001.iso`, `2C_AUDIO__TRACK002.iso`, ...
- Multichannel: `MC_AUDIO__TRACK001.iso`, `MC_AUDIO__TRACK002.iso`, ...

## Supported Formats

- **File extensions**: `.iso`, `.dat`
- **Sector sizes**: 2048 bytes (standard) and 2064 bytes (raw/physical)
- **Frame formats**: DSD raw, DST encoded (lossless compression)
- **Hybrid discs**: Supported (CD layer is ignored)

## Technical Notes

### Audio Format

- Sample rate: 2,822,400 Hz (DSD64)
- Output format: Native DSD (SampleFormat::DSD)
- Channels: 2 (stereo) or up to 6 (multichannel)

### DST Decoding

DST (Direct Stream Transfer) is a lossless compression algorithm used on most SACDs
to fit both stereo and multichannel audio on a single disc. This plugin uses FFmpeg's
`AV_CODEC_ID_DST` decoder for decompression, which is a well-tested implementation
based on ISO/IEC 14496-3 Part 3 Subpart 10.

### Memory Usage

- Master TOC: ~20KB per disc
- Frame buffer: 64KB
- DST decoder state: ~1MB (managed by FFmpeg)

### Seeking

Seeks to frame boundaries with 1/75 second (13.3ms) accuracy.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    SacdIsoDecoderPlugin                     │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │  SacdDisc   │  │  SacdMedia  │  │    DstDecoder       │  │
│  │  (parser)   │  │  (file I/O) │  │  (FFmpeg wrapper)   │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
│         │                │                    │              │
│         ▼                ▼                    ▼              │
│  ┌─────────────────────────────────────────────────────────┐│
│  │              ScarletBook.hxx (structures)               ││
│  └─────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌─────────────────┐
                    │  FFmpeg         │
                    │  libavcodec     │
                    │  (DST decoder)  │
                    └─────────────────┘
```

## Code Quality

This plugin follows MPD upstream coding standards:
- Modern C++23 with RAII memory management
- Exception-based error handling where appropriate
- CamelCase naming conventions
- Uses existing MPD/FFmpeg infrastructure
- No additional external dependencies

## Files

```
src/lib/sacdiso/
├── Domain.hxx/cxx        # Logging domain
├── ScarletBook.hxx       # SACD format structures
├── SacdMedia.hxx/cxx     # File/stream I/O abstraction
├── SacdDisc.hxx/cxx      # ISO parser and frame reader
├── DstDecoder.hxx/cxx    # FFmpeg DST decoder wrapper
├── meson.build           # Build configuration
└── README.md             # This file

src/decoder/plugins/
├── SacdIsoDecoderPlugin.hxx
└── SacdIsoDecoderPlugin.cxx
```

## License

GPL-2.0-or-later (same as MPD)

## Credits

- SACD format structures based on Scarlet Book specification
- DST decoding via FFmpeg (Peter Ross, et al.)
- Parsing logic informed by libsacd and sacd-ripper projects
