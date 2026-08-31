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
`ffmpeg-N-126313-g1ae4048218-win64-lgpl-shared.zip` asset from the
`autobuild-2026-08-29-13-12` BtbN/FFmpeg-Builds release. The script verifies:

- checksum manifest SHA-256:
  `3D9D4AAF0D4B1A9CB28F36847680E5C585A71FE29F40E5F948D211E8735BE056`;
- FFmpeg archive SHA-256, read from that verified manifest:
  `353A95C745D10E52F0E32798D0A72DDE2C1F387D3C22F40EC94BF953ADA5E6FE`.

The selected BtbN variant is the LGPL shared build. The required shared
libraries remain replaceable and their included license is copied into the
user's private `out` directory. FFmpeg and BtbN retain all applicable rights
and notices.
