# Embedding: building the library inside another project

This is how a parent project compiles libprometheia from a checkout of this
repository, typically a git submodule. It is written for parents with their
own build system: plain make, or a compiler script with no CMake. The
library's C interface itself is [C_API.md](C_API.md); this document covers
only how to build it.

There are two supported ways in:

1. **The source list (embed contract 1).** The parent compiles the files
   that `embed/sources.txt` lists, with the obligations below. With GNU make
   it can include `embed/prometheia.mk`, which reads the list and builds the
   archive. **This is the way to use when the parent does not use CMake.**
   It is the only way that serves a make build and a compiler script from
   the same list.
2. **CMake.** The parent runs this repository's CMake into a build
   directory of its own, then links `libprometheia.a` with the
   `prometheia.pc` it writes (`PKG_CONFIG_PATH=<that dir> pkg-config
   --static --cflags --libs prometheia`). This needs CMake ≥ 3.20 and
   pkg-config wherever the parent builds.

Both produce the same library. `CMakeLists.txt` builds `libprometheia` from
`embed/sources.txt` itself, so the list cannot fall behind what CMake
compiles.

## The contract (number 1)

`embed/sources.txt` carries a line `contract 1`. Everything in this section
is what the number stands for. The number is raised when, and only when, a
parent has to do something new beyond compiling the list.

- **Sources:** every line of `embed/sources.txt` that begins with `src/`,
  relative to the repository root. All are C++. Read the list; don't copy
  it. Files can be added, removed or renamed in any commit without a new
  contract number, and a parent that reads the list needs no change.
- **Include directory:** `include/`, which holds `<prometheia/prometheia.h>`
  (the C interface) and the C++ headers. The parent's own code that calls
  the library needs it too. Nothing else is needed: the library's private
  `.inc` and `.hpp` files sit beside the sources that include them.
- **Language:** ISO C++20, with no compiler extensions (`-std=c++20`, not
  `gnu++20`).
- **Floating point, required:**
  - No fused multiply-add contraction: `-ffp-contract=off` with GCC or
    Clang. Clang contracts `a*b+c` by default, and arm64 (Apple silicon)
    has the instruction. Compiling `src/frames.cpp` for aarch64 at `-O2`
    gave 459 fused instructions with contraction on and none with it off.
    That would change results by platform.
  - No `-ffast-math`, `-Ofast` or any `-funsafe-math-*` flag.
  - With MSVC: `/fp:precise`, which is the default, and never `/fp:fast`
    or `/fp:contract`. Microsoft documents that from Visual Studio 2022
    (17.0) on, `/fp:precise` generates no contractions on any platform.
  - `src/frames.cpp` refuses to compile when the compiler says fast math
    or contraction is on (`__FAST_MATH__`, `_M_FP_FAST`, `_M_FP_CONTRACT`).
    Clang's default contraction defines no macro, so for Clang the
    requirement above has to be followed by hand.
- **Optimisation level:** any. The library's answers do not depend on it,
  measured with GCC 11.4 and Clang 14 on x86-64 with libstdc++:
  `embedtest.py` builds at `-O2`, and its 94 CSV rows at `%.17g` are
  identical to CMake's `-O3` build.
- **Defines:** none. The library has no `assert`, so `NDEBUG` changes
  nothing. `PROMETHEIA_PORTABLE_FALLBACKS` is for testing only: it selects
  on Linux the code paths MSVC and Apple's libc++ take (see "Platforms").
- **Generated files:** none. The one data file compiled in, the shipped
  hypothetical-body set, is committed as `src/hypotheticals_shipped.inc`
  ([HYPOTHETICALS.md](HYPOTHETICALS.md)).
- **Dependencies:**
  - `libzstd`: its header for `src/catalog.cpp`, and the library at link
    time. It stays an ordinary external dependency on every platform: the
    distribution's package on Linux, vcpkg on Windows, Homebrew on macOS.
    Point `PROMETHEIA_ZSTD_CFLAGS` and `PROMETHEIA_ZSTD_LIBS` at it when it
    is off the default paths.
  - The C++ standard library and `libm`.
  - No threads, sockets or other system libraries.
- **Link:** with the C++ compiler driver, or add the C++ runtime yourself.
- **Data is not in the checkout.** The ephemeris files are fetched and
  verified by `tools/fetch/de_fetch.py` (`--only de440|de441|sb441`,
  `--verify`); see [DE.md](DE.md).

A parent can refuse to build against a contract it does not know.
`prometheia.mk` exports the number for this. A compiler script reads the
same `contract` line.

## With GNU make: `embed/prometheia.mk`

This needs GNU make 3.81 or later; `sed` and `grep` must be on `PATH`.

```make
PROMETHEIA_DIR := third_party/prometheia
PROMETHEIA_OBJDIR := obj-prometheia
include $(PROMETHEIA_DIR)/embed/prometheia.mk
ifneq ($(PROMETHEIA_EMBED_CONTRACT),1)
$(error Prometheia embed contract is $(PROMETHEIA_EMBED_CONTRACT); this build knows 1)
endif
CPPFLAGS += $(PROMETHEIA_CPPFLAGS)

app: app.o $(PROMETHEIA_LIB)
	$(CXX) -o $@ app.o $(PROMETHEIA_LIB) $(PROMETHEIA_LDLIBS)
```

The fragment defines `PROMETHEIA_SRCS`, `PROMETHEIA_CPPFLAGS`,
`PROMETHEIA_CXXFLAGS` (the floating-point and language flags above) and
`PROMETHEIA_LDLIBS`. When `PROMETHEIA_OBJDIR` is set, it also defines
`PROMETHEIA_OBJS`, `PROMETHEIA_LIB` (`$(PROMETHEIA_OBJDIR)/libprometheia.a`)
and their rules, with `-MMD` dependency tracking. Its header comment lists
every variable.

Three properties are designed in and tested:

- **The parent's `CXXFLAGS` are never applied to the library.** A
  `-ffast-math` in the parent cannot reach it. Use `PROMETHEIA_OPTFLAGS`
  (default `-O2`) and `PROMETHEIA_EXTRA_CXXFLAGS` (`-g`, sanitizers,
  `-fPIC`) to change how the library itself is built.
- **Including the fragment leaves the parent's default goal unchanged.**
- **zstd off the default paths** is reached with `PROMETHEIA_ZSTD_CFLAGS`
  and `PROMETHEIA_ZSTD_LIBS` (default `-lzstd`).

Give `PROMETHEIA_OBJDIR` a directory of its own. The library's object names
(`engine.o`, `time.o` and so on) are generic, and the rule that builds them
is the pattern `$(PROMETHEIA_OBJDIR)/%.o`.

## Proof: `tools/check/embedtest.py`

The test writes a parent makefile into a scratch directory. The makefile
includes the fragment before its own first target and sets a poisoned
`CXXFLAGS` (`-ffast-math` and a marker define). It then builds
`tools/ephem/ephem.c`, the strict-C99 consumer of the C interface. Six
assertions follow:

| assertion | fires when |
|---|---|
| `no-contract` | the fragment could not read the contract line |
| `sources-differ` | the fragment's list differs from the manifest's |
| `goal-taken` | the fragment took the default goal |
| `flags-leaked` | the parent's flags reached a library compile line |
| `build-failed` | the build failed |
| `numbers-differ` | the answers differ from `build/ephem`'s on DE440 |

The `numbers-differ` comparison runs eleven queries, 94 CSV lines. The
queries cover planets, a span of instants, 1600 and 2400, a topocentric
sidereal position, heliocentric, equatorial J2000, orbit points, every
shipped hypothetical body, two stars, and three catalog bodies read
through zstd.

Each assertion was shown to fire by a fault injected into the fragment:
- passing the parent's `CXXFLAGS` through;
- `-ffast-math` in the library (which `src/frames.cpp` now also refuses
  to compile);
- not restoring the default goal;
- dropping a source;
- a wrong manifest path.

`tools/scheduled.sh` runs the test three times, about six seconds each on
this machine, which is too long for the gate:
- with GCC;
- with Clang 14, when `clang++-14` is present;
- with GCC and `--extra-cxxflags=-DPROMETHEIA_PORTABLE_FALLBACKS`. This
  run builds the non-GCC code paths. Its 94 rows equal the CMake build
  too. A one-ulp fault injected into each fallback in turn fires
  `numbers-differ`, which proves the queries reach both.

## Platforms

- **Linux** (GCC 11.4, Clang 14, x86-64): built and measured as above.
- **Linux, GCC 13.3** (Ubuntu 24.04, in a container, 2026-10-25): all 14
  sources compile with `-Wall -Wextra -Werror`. `ephem` built through
  `prometheia.mk` gives the same 41 CSV lines as our GCC 11 build.
- **Windows (MSVC) and macOS (Apple clang)**: **never built.** A
  read-through audit and a strict-mode MinGW compile found these blockers,
  and each has been removed:
  - `M_PI`, which is not ISO C++ and needs `_USE_MATH_DEFINES` on MSVC. The
    library uses its own constants now.
  - The GCC vector extension in the two-node nutation kernel
    (`src/frames.cpp`). Other compilers get a two-lane struct doing the same
    operations in the same order.
  - Floating-point `std::from_chars` (`src/hypotheticals.cpp`), which
    Apple's libc++ long lacked. Where `__cpp_lib_to_chars` is not
    advertised, a classic-locale stream reads the number.
  - Floating-point `std::to_chars` and `std::from_chars` in the registry's
    `widened()` (`src/registry.cpp`, A-9), which had no such guard.
    Astrolog's macOS job failed on them (run 40640400077, our `2fdab7d`,
    Xcode 26.6 SDK, `MACOSX_DEPLOYMENT_TARGET=12.0`): Apple's libc++ marks
    them unavailable below macOS 13.3 and 26.0. They now sit behind the
    same guard, and the fallback finds the shortest decimal with
    classic-locale streams. It equals `to_chars` bit for bit on 2.2
    million floats, random bit patterns included. `test_widened` checks
    both paths in the gate: it is built a second time with
    `PROMETHEIA_PORTABLE_FALLBACKS`, and a fault injected into the fallback
    fails only that build.
  - Catalog offsets through a 32-bit `long` on Windows. The DE, SPK and
    catalog readers now share `io::RandomAccessFile` (`src/io.cpp`), which
    reads at 64-bit offsets: `pread` on POSIX, `ReadFile` with an
    `OVERLAPPED` offset on Windows, where the UTF-8 path is widened for
    `CreateFileW`, so a path outside the ANSI code page opens. That Windows
    branch has not been built with MSVC yet. Astrolog compiled it with
    mingw-w64 (`-std=c++20 -Wall`, no warnings), against our `a11531e`, and
    ran it under Wine. The harness read a 1,280,268-byte `.epm` in a
    directory named `Ünïcödé-ε` at three offsets, plus a short read at the
    end and a missing file. Its output was identical to the same harness
    built natively on Linux. Plain `fopen` of that path fails under Wine.
    Wine is not Windows, and that run covered no read over 1 GiB and no
    second thread.
  - **MSVC** built the readers at our `2fdab7d` on Astrolog's release dry
    run 40640400077 (GitHub's Windows runner, Qt 6.8.3). Every source
    compiled, `io.cpp` and `registry.cpp` with no warning. Their suite
    passed 6,477 checks with none failing, and its 71 lines carrying
    numbers equal a Linux GCC 11 run to the printed digit. The catalog-body
    lines were compared across two catalog files, the full SBDB one on
    Linux and Astrolog's shipped subset on Windows.
  - **The writers and the remaining opens** took the path in the ANSI code
    page until 2026-10-28: the catalog writer (`fopen`), `de::trim` and
    `spk::write_spk` (`std::ofstream`), the engine's sniff of an
    ephemeris's kind, and `add_hypotheticals` (`std::ifstream`). So a
    cutter could not write to a non-ASCII path, and `Engine::open` could
    not open one, though the readers behind it could. They now go through
    `io::open_file` and `io::native_path`, which widen the UTF-8 path on
    Windows, and through `RandomAccessFile`. Built with mingw-w64 (GCC 10)
    and run under Wine: `open_file` wrote and `RandomAccessFile` read back a
    file at `C:\users\Public\Ünïcödé-ε\éphéméride.bin`, where a narrow
    `fopen` of the same path fails.
  - Headers reached only transitively. The first pass added `<cstdint>`
    where the audit named it and missed `src/frames.cpp`. GCC 13's
    libstdc++ no longer pulls `<cstdint>` in, so Astrolog's Ubuntu 24.04
    build failed there. `clang-include-cleaner` then added every standard
    header the library uses but did not include directly: 51 lines in 18
    files.

  The two fallbacks are built and checked on Linux (above).
  - **MSVC 2022** compiled all 14 sources with no errors (Astrolog's
    Windows job, 2026-10-25): `cl /std:c++20 /permissive- /Zc:__cplusplus
    /EHsc /MD /O2 /W3`, against vcpkg's `zstd:x64-windows-static-md`. The
    only warnings were C4996, the CRT's deprecation of `fopen` and
    `strncpy`.
  - **Run on Windows** (Astrolog's release dry run 6, 2026-10-25, our
    `624c96d`, Qt 6.8.3 on a GitHub Windows runner), against the shipped
    data: the 1900–2100 DE441 cut, a 13,808-body catalog, and the 1900–2100
    cut of sb441-n16.
    - Their suite passed 6,469 checks with none failing.
    - Their 63 oracle lines for the Prometheia legs are identical to the
      same legs on Linux (GCC 11), printed to 0.0001″. The legs cover
      bodies at three epochs, topocentric, heliocentric, sidereal, nodes and
      apogees under several masks, a catalog body, a star, and UT1.
    - That is agreement at four decimals of an arcsecond, not a
      bit-for-bit comparison of the doubles, which nobody has made.
  - **Apple clang, arm64**: Astrolog's macOS leg in the same run was green.
    No numbers from it have been compared here. `CMakeLists.txt` passes
  GCC-style flags and needs pkg-config for zstd, so on Windows the source
  list is the way in, not CMake.
- **Across platforms**, `sin`, `cos`, `atan2` and the like come from each
  platform's own maths library and may differ in the last ulp. Compare
  answers between platforms with a tolerance, not bit for bit.
