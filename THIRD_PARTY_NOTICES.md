# Third-party notices

## PSPRecomp

This source distribution includes a project-specific modified PSPRecomp
snapshot under `PSPRecomp/`. PSPRecomp is distributed under the MIT License;
its license and notices are retained in that directory.

## PPSSPP

PPSSPP is licensed under GPL-2.0-or-later. It is not bundled with this project.
The user supplies an official portable Windows x64 ZIP, used locally only for
PPSSPP's official decrypted-EBOOT dump function. Any independent
redistribution of PPSSPP must comply with its license and source obligations.

## FFmpeg

FFmpeg is not bundled in the repository or source-only release ZIP. During a
local build, `tools/ensure_ffmpeg.ps1` downloads the pinned
`ffmpeg-N-126574-g912208af28-win64-lgpl-shared.zip` asset from the immutable
`autobuild-2026-09-15-13-18` BtbN/FFmpeg-Builds release. The archive SHA-256
is pinned directly in the script, so no separate checksum-manifest request is
needed. The script verifies:

- FFmpeg archive SHA-256:
  `D57B4540DEBEBB12F6BAD7D04D7D450FBCE7822E04DDABA6182E2C8FC31955D0`.

The selected BtbN variant is the LGPL shared build. The required shared
libraries remain replaceable and their included license is copied into the
user's private `out` directory. FFmpeg and BtbN retain all applicable rights
and notices.

## LLVM-MinGW

Visual Studio is not required. During a local build,
`tools/ensure_toolchain.ps1` downloads the pinned official
`llvm-mingw-20260908-ucrt-x86_64.zip` archive from the llvm-mingw 20260908
release and verifies this SHA-256 before extraction:

- LLVM-MinGW archive SHA-256:
  `1BCF74D06B724AEecAA6412CA85F5B26FB1DA770E7CDCEFA9263C9C5C3AD34B6`.

The archive remains in the user's ignored `tools/.build` directory and is not
part of the repository or release ZIP. LLVM-MinGW combines LLVM/Clang with
mingw-w64 components; their upstream licenses and notices apply. Source and
license information is available from the official llvm-mingw release and its
corresponding source repositories.
