#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Out-of-tree install check of the Passerelle package (gap 38).

The install rules are only worth something if a project that knows nothing
about this source tree can use the installed result. This script proves it,
in a throwaway prefix, without touching the tree it is run from:

  1. `cmake --install` puts the libraries, the headers, the pkg-config file
     and the CMake package into a temporary prefix;
  2. the public headers are there, under their `pgw/` prefix;
  3. `passerelle.pc` parses (every mandatory field is present) and its version
     is the version of the VERSION file;
  4. `pkg-config --cflags --libs` returns usable flags, and a small C
     consumer compiled with exactly those flags builds, links and runs;
  5. the same consumer, built by CMake with `find_package(Passerelle)`,
     configures, builds and runs: the exported CMake package is real.

Nothing is skipped silently: a missing pkg-config or a missing compiler is a
failure, because the claim "a downstream project can consume the install" is
then not proven.

Usage:
    python3 tests/install/install_check.py --build-dir build --source-dir .

Exit status: 0 when every step passed, 1 on the first failure.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

# The public contract of the installed package: those headers must be there.
REQUIRED_HEADERS = (
    "pgw/buf.h",
    "pgw/err.h",
    "pgw/limits.h",
    "pgw/log.h",
    "pgw/time.h",
    "pgw/p9.h",
    "pgw/fusep.h",
    "pgw/virtio.h",
    "pgw/rmfs.h",
)

# The fields a .pc file must carry (pkg-config itself ignores the others).
REQUIRED_PC_FIELDS = ("Name", "Description", "Version", "Libs", "Cflags")

EXPECTED_OUTPUT = "consumer: pgw_get_u16 -> 0x1234"


class CheckError(Exception):
    """One step of the install check failed."""


def run(cmd, cwd=None, env=None):
    """Run a command, capture its output and raise on a non-zero status."""
    proc = subprocess.run(cmd, cwd=cwd, env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        raise CheckError("command failed (%d): %s\n%s"
                         % (proc.returncode, " ".join(map(str, cmd)), proc.stdout))
    return proc.stdout


def install_into(build_dir, prefix):
    """Install the build tree into `prefix` (a temporary directory)."""
    run(["cmake", "--install", str(build_dir), "--prefix", str(prefix)])


def check_headers(prefix):
    """Check that every public header was installed under include/pgw."""
    incdir = prefix / "include"
    missing = [h for h in REQUIRED_HEADERS if not (incdir / h).is_file()]
    if missing:
        raise CheckError("headers not installed in %s: %s" % (incdir, missing))


def parse_pc(path):
    """Parse a pkg-config file into a dictionary of its fields."""
    fields = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or ":" not in line:
            continue
        key, _, value = line.partition(":")
        fields[key.strip()] = value.strip()
    return fields


def find_pc(prefix):
    """Return the path of the installed passerelle.pc."""
    for libdir in ("lib", "lib64"):
        pcfile = prefix / libdir / "pkgconfig" / "passerelle.pc"
        if pcfile.is_file():
            return pcfile
    raise CheckError("passerelle.pc not found under %s" % prefix)


def check_pc(pcfile, version):
    """Check the fields and the version of the installed .pc file."""
    fields = parse_pc(pcfile)
    missing = [f for f in REQUIRED_PC_FIELDS if f not in fields]
    if missing:
        raise CheckError("%s: missing field(s) %s" % (pcfile, missing))
    if fields["Version"] != version:
        raise CheckError("%s: version is %r, VERSION file says %r"
                         % (pcfile, fields["Version"], version))


def pkg_config_flags(pcfile):
    """Return the (cflags, libs) of the installed package, via pkg-config."""
    if shutil.which("pkg-config") is None:
        raise CheckError("pkg-config is not installed: the .pc file could not "
                         "be proven usable (install pkg-config and retry)")
    env = dict(os.environ)
    env["PKG_CONFIG_PATH"] = str(pcfile.parent)
    cflags = run(["pkg-config", "--cflags", "passerelle"], env=env).split()
    libs = run(["pkg-config", "--libs", "passerelle"], env=env).split()
    if not any(f.startswith("-I") for f in cflags):
        raise CheckError("pkg-config --cflags gave no include flag: %s" % cflags)
    if not any(f.startswith("-L") for f in libs):
        raise CheckError("pkg-config --libs gave no library flag: %s" % libs)
    return cflags, libs


def sanitizer_flags(build_dir):
    """Return the sanitizer flags of the build tree, if it was instrumented."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return []
    text = cache.read_text(encoding="utf-8", errors="replace")
    if "PGW_SANITIZERS:BOOL=ON" in text:
        return ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    return []


def compile_consumer(cc, source, outdir, cflags, libs, extra):
    """Compile the consumer with the pkg-config flags and run it."""
    binary = outdir / "consumer-pkgconfig"
    run([cc] + list(cflags) + [str(source)] + list(libs) + list(extra)
        + ["-o", str(binary)])
    out = run([str(binary)])
    if EXPECTED_OUTPUT not in out:
        raise CheckError("the consumer did not print %r: %s" % (EXPECTED_OUTPUT, out))


def build_cmake_consumer(cc, consumer_dir, prefix, outdir, extra):
    """Configure, build and run the consumer that uses find_package()."""
    build = outdir / "consumer-cmake"
    flags = " ".join(extra)
    conf = run(["cmake", "-S", str(consumer_dir), "-B", str(build),
                "-DCMAKE_PREFIX_PATH=%s" % prefix,
                "-DCMAKE_C_COMPILER=%s" % cc,
                "-DCMAKE_C_FLAGS=%s" % flags,
                "-DCMAKE_EXE_LINKER_FLAGS=%s" % flags])
    if "Passerelle_VERSION=" not in conf:
        raise CheckError("find_package(Passerelle) did not report a version")
    run(["cmake", "--build", str(build)])
    binary = build / "pgw_consumer"
    if not binary.is_file():
        raise CheckError("the CMake consumer did not produce %s" % binary)
    out = run([str(binary)])
    if EXPECTED_OUTPUT not in out:
        raise CheckError("the CMake consumer did not print %r: %s"
                         % (EXPECTED_OUTPUT, out))


def parse_args(argv=None):
    """Parse the command line."""
    ap = argparse.ArgumentParser(description="Install the tree in a temporary "
                                             "prefix and consume it.")
    ap.add_argument("--source-dir", default=".", help="repository root")
    ap.add_argument("--build-dir", required=True, help="build tree to install")
    ap.add_argument("--consumer-dir", default=None,
                    help="directory of the CMake consumer project")
    ap.add_argument("--cc", default=None, help="C compiler (default: cc)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the temporary prefix for inspection")
    return ap.parse_args(argv)


def main(argv=None):
    """Run the whole check and report the first failure."""
    args = parse_args(argv)
    source = pathlib.Path(args.source_dir).resolve()
    build = pathlib.Path(args.build_dir).resolve()
    consumer = pathlib.Path(args.consumer_dir).resolve() if args.consumer_dir \
        else source / "tests" / "install" / "consumer"
    cc = args.cc or shutil.which("cc") or "cc"
    version = (source / "VERSION").read_text(encoding="utf-8").strip()
    prefix = pathlib.Path(tempfile.mkdtemp(prefix="passerelle-install-"))
    try:
        install_into(build, prefix)
        check_headers(prefix)
        pcfile = find_pc(prefix)
        check_pc(pcfile, version)
        cflags, libs = pkg_config_flags(pcfile)
        extra = sanitizer_flags(build)
        compile_consumer(cc, consumer / "consumer.c", prefix, cflags, libs, extra)
        build_cmake_consumer(cc, consumer, prefix, prefix, extra)
    except CheckError as err:
        print("install-check: FAILED: %s" % err, file=sys.stderr)
        return 1
    finally:
        if args.keep:
            print("install-check: prefix kept at %s" % prefix)
        else:
            shutil.rmtree(prefix, ignore_errors=True)
    print("install-check: the installed prefix was consumed successfully "
          "(headers, pkg-config, CMake package)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
