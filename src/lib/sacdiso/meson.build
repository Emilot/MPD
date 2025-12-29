# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright The Music Player Daemon Project

# libdstdec - New DST decoder implementation (MPD code standards)
libdstdec_sources = files(
  'libdstdec/decoder/decoder.cpp',
  'libdstdec/binding/dst_decoder.cpp',
)

sacdiso_sources = files(
  'Domain.cxx',
  'SacdMedia.cxx',
  'SacdDisc.cxx',
  'DstDecoder.cxx',
)

sacdiso_dependencies = [
  log_dep,
]

# Check for iconv (needed for character set conversion)
if not is_windows
  if c_compiler.has_function('iconv_open')
    conf.set('HAVE_ICONV', true)
  else
    iconv_dep = dependency('iconv', required: false)
    if iconv_dep.found()
      conf.set('HAVE_ICONV', true)
      sacdiso_dependencies += iconv_dep
    endif
  endif
endif

# Build libdstdec
libdstdec = static_library(
  'dstdec',
  libdstdec_sources,
  include_directories: inc,
  dependencies: [log_dep],
)

libdstdec_dep = declare_dependency(
  link_with: libdstdec,
)

sacdiso = static_library(
  'sacdiso',
  sacdiso_sources,
  include_directories: inc,
  dependencies: sacdiso_dependencies + [libdstdec_dep],
)

sacdiso_dep = declare_dependency(
  link_with: sacdiso,
  dependencies: sacdiso_dependencies,
)
