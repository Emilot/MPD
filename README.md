## Summary

New decoder plugin for native playback of SACD (Super Audio CD) ISO images.

## Motivation

SACD ISO files are a common distribution format for high-resolution DSD audio ripped from Super Audio CDs. This plugin allows MPD to play these files natively, outputting DSD64 directly to compatible hardware.

## Features

- **Native DSD output**: 2.8224 MHz (DSD64) to compatible DACs
- **Dual area support**: Both 2-channel stereo and multichannel surround
- **DST decoding**: Integrated libdstdec based on ISO/IEC 14496-3 reference implementation
- **Container scanning**: ISO appears as virtual directory with individual tracks
- **Full metadata**: Album title, artist, track names extracted from disc TOC
- **Seeking**: Frame-accurate seeking within tracks

## Configuration
```conf
decoder {
    plugin "sacdiso"
    edited_master "false"
    playable_area "both"
    lsbitfirst "false"
}
```

## Implementation Notes

- Uses modern C++23 with RAII memory management
- DST decoder is self-contained (no FFmpeg dependency)
- Follows MPD coding conventions (CamelCase, exception handling)
- Supports both 2048-byte (standard) and 2064-byte (raw) sector sizes

## Files Added

- `src/lib/sacdiso/` - SACD parsing library
- `src/lib/sacdiso/libdstdec/` - DST decoder
- `src/decoder/plugins/SacdIsoDecoderPlugin.cxx`

## Testing

Tested with various SACD ISO files including DST-encoded and raw DSD content, stereo and multichannel areas.

# Music Player Daemon

http://www.musicpd.org

A daemon for playing music of various formats.  Music is played through the 
server's audio device.  The daemon stores info about all available music, 
and this info can be easily searched and retrieved.  Player control, info
retrieval, and playlist management can all be managed remotely.

For basic installation instructions
[read the manual](https://www.musicpd.org/doc/user/install.html).

# Users

- [Manual](https://mpd.readthedocs.io/en/stable/user.html), or see `/usr/share/doc/mpd/html` in your installation
- [Forum](https://github.com/MusicPlayerDaemon/MPD/discussions)
- [IRC](ircs://irc.libera.chat:6697/#mpd)
- [Bug tracker](https://github.com/MusicPlayerDaemon/MPD/issues/)

# Developers

- [Protocol specification](https://mpd.readthedocs.io/en/latest/protocol.html)
- [Developer manual](https://mpd.readthedocs.io/en/latest/developer.html)

# Legal

MPD is released under the
[GNU General Public License version 2](https://www.gnu.org/licenses/gpl-2.0.txt),
which is distributed in the COPYING file.
