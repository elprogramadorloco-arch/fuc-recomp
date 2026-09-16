# Standalone public build boundary

`run.bat` is relocatable. Its installation root is always the directory that
contains `run.bat`; the directory may have any name and may be located anywhere
the user can write. No build step may search a parent directory for unpublished
FateUnlimitedCodesRecomp sources.

The source-only package contains three independent source areas:

- `PSPRecomp/`: the project-specific modified static-recompilation framework;
- `native/`: the original PSP host, HLE, media bridge, audio, input and renderer
  sources required to link the corpus generated from the user's EBOOT;
- `src/`: the launcher and ISO preparation tool.

User-provided files exist only under `data/`. Generated private files exist only
under `out/` and `tools/.build/`, both beside `run.bat`. The public repository
and release ZIP must never contain those generated folders or any user input.

FFmpeg is not vendored. `tools/ensure_ffmpeg.ps1` downloads a pinned BtbN
Windows x64 LGPL shared development archive, verifies a pinned checksum
with SHA-256, retries transient network failures through `curl.exe` or
PowerShell, and stages only the required replaceable DLLs and license in the
user's private output. The expected archive hash is stored directly in the
script, so the build does not depend on a second checksum-manifest request.

Visual Studio is not used by the public build. The scripts select the normal
system CMake installation (preferring `C:\Program Files\CMake\bin\cmake.exe`)
and require CMake 3.22 or newer. `tools/ensure_toolchain.ps1` downloads the
pinned official LLVM-MinGW x64 archive into ignored local build storage,
verifies its SHA-256, and CMake drives that compiler through the `MinGW
Makefiles` generator. Neither the compiler archive nor its extracted files are
published in the repository or source-only release ZIP.

Before every release, `tools/verify_public_tree.ps1` verifies both the legal
source-only boundary and the presence of the standalone native source set. The
exact release tree must also be tested from a directory whose name is not
`public`. These checks guard against the v0.0.1 regression where the build
looked for native sources in an unpublished parent repository and accidentally
constructed a sibling `public` path.
