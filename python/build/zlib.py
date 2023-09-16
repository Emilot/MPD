import subprocess
from typing import Optional

from build.makeproject import MakeProject

class ZlibProject(MakeProject):
    def __init__(self, url: str, md5: str, installed: str,
                 **kwargs):
        MakeProject.__init__(self, url, md5, installed, **kwargs)

    def get_make_args(self, toolchain) -> list[str]:
        return MakeProject.get_make_args(self, toolchain) + [
            'CC=' + toolchain.cc + ' ' + toolchain.cppflags + ' ' + toolchain.cflags,
            'CPP=' + toolchain.cc + ' -E ' + toolchain.cppflags,
            'AR=' + toolchain.ar,
            'ARFLAGS=' + toolchain.arflags,
            'RANLIB=' + toolchain.ranlib,
            'LDSHARED=' + toolchain.cc + ' -shared',
            'libz.a'
        ]

    def get_make_install_args(self, toolchain) -> list[str]:
        return [
            'RANLIB=' + toolchain.ranlib,
            self.install_target
        ]

    def _build(self, toolchain) -> None:
        src = self.unpack(toolchain, out_of_tree=False)

        subprocess.check_call(['./configure', '--prefix=' + toolchain.install_prefix, '--static'],
                              cwd=src, env=toolchain.env)
        self.build_make(toolchain, src)
