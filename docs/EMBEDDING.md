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
- **Optimisation level:** any. The library's answers do not depend on it,
  measured with GCC 11.4 and Clang 14 on x86-64 with libstdc++:
  `embedtest.py` builds at `-O2`, and its 94 CSV rows at `%.17g` are
  identical to CMake's `-O3` build.
- **Defines:** none. The library has no `assert`, so `NDEBUG` changes
  nothing.
- **Generated files:** none. The one data file compiled in, the shipped
  hypothetical-body set, is committed as `src/hypotheticals_shipped.inc`
  ([HYPOTHETICALS.md](HYPOTHETICALS.md)).
- **Dependencies:**
  - `libzstd`: its header for `src/catalog.cpp`, and the library at link
    time.
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
- `-ffast-math` in the library;
- not restoring the default goal;
- dropping a source;
- a wrong manifest path.

`tools/scheduled.sh` runs the test with GCC, and again with Clang 14 when
`clang++-14` is present. That takes about six seconds per compiler on this
machine, which is too long for the gate.

## Platforms

- **Linux** (GCC 11.4, Clang 14, x86-64): built and measured as above.
- **Windows (MSVC) and macOS (Apple clang)**: never built. A read-through
  audit and a strict-mode MinGW compile found these blockers:
  - `M_PI` used without `_USE_MATH_DEFINES` (`src/engine.cpp`,
    `src/natural_apsides.hpp`);
  - a GCC vector extension in the two-node nutation kernel
    (`src/frames.cpp`);
  - floating-point `std::from_chars` (`src/hypotheticals.cpp`), which
    Apple's libc++ is believed to lack.

  Until they are fixed and a build on each platform has been run, nothing
  in this document is claimed for either.
