# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds prometheia._native against an installed libprometheia, found by
pkg-config (PKG_CONFIG_PATH=build for the build tree). Nothing is bundled.
Without Cython, or without the library, the package is the pure-Python
client alone, and says so; PROMETHEIA_NATIVE=1 makes that an error
(docs/PYTHON.md, "Native")."""
import os
import shlex
import subprocess

from setuptools import Extension, setup


def pkg_config(*args):
    out = subprocess.run(["pkg-config", *args, "prometheia"], check=True, capture_output=True,
                         text=True).stdout
    return shlex.split(out)


def native():
    from Cython.Build import cythonize

    cflags, libs = pkg_config("--cflags"), pkg_config("--libs")
    # The static archive is a dependency too, so a rebuilt library relinks.
    archives = [os.path.join(f[2:], "libprometheia.a") for f in libs if f.startswith("-L")]
    ext = Extension(
        "prometheia._native", ["prometheia/_native.pyx"],
        include_dirs=[f[2:] for f in cflags if f.startswith("-I")],
        extra_compile_args=[f for f in cflags if not f.startswith("-I")],
        library_dirs=[f[2:] for f in libs if f.startswith("-L")],
        libraries=[f[2:] for f in libs if f.startswith("-l")],
        extra_link_args=[f for f in libs if not f.startswith(("-L", "-l"))],
        depends=[a for a in archives if os.path.exists(a)])
    return cythonize([ext], compiler_directives={"language_level": "3"})


def ext_modules():
    try:
        return native()
    except (ImportError, OSError, subprocess.CalledProcessError) as e:
        if os.environ.get("PROMETHEIA_NATIVE") == "1":
            raise
        print("prometheia: building without the native extension (%s)" % e)
        return []


setup(ext_modules=ext_modules(),
      package_data={"prometheia": ["cprometheia.pxd", "_native.pyx", "_native.pyi", "py.typed"]})
