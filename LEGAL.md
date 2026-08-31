# Legal scope

This repository component is an interoperability-oriented launcher and local
preparation tool. It is not legal advice.

## Not included

Do not commit or distribute:

- the game ISO or any extracted game files;
- encrypted or decrypted EBOOT files;
- generated static-recompilation source or object code derived from the game;
- game audio, video, textures, models, fonts, save states, or save data;
- PPSSPP binaries copied from a downloaded archive;
- locally built executables that embed generated game code unless counsel or
  the applicable rightsholder has confirmed that distribution is authorized.

The preparation script requires every user to supply their own inputs and
performs extraction, EBOOT dumping and static recompilation locally. The final
`out` directory is private and must not be uploaded. Ownership of an original copy does not automatically grant
permission to redistribute copyrighted game content or derivative binaries.

## Third-party projects

- PSPRecomp is a build-time framework under the MIT License. Keep its license
  notice when redistributing its source or covered binary portions. The exact
  framework snapshot used by this pipeline is included under `PSPRecomp/`.
- PPSSPP is licensed under GPL-2.0-or-later. This project does not bundle it;
  the user supplies an official archive. If you choose to redistribute PPSSPP,
  comply independently with its license and source-code obligations.
- FFmpeg is not stored in this repository. The local build downloads a pinned
  Windows x64 LGPL shared development package from BtbN/FFmpeg-Builds, verifies
  its checksum manifest and archive with SHA-256, and copies the required
  replaceable shared libraries plus their license only into the user's private
  `out` directory. FFmpeg and BtbN retain their respective rights and notices.

## Standalone public build boundary

The original PSP-native host/HLE and renderer sources needed to link the
user-generated corpus are intentionally included under `native/`. They contain
no ISO, EBOOT, generated AOT corpus, fonts, audio, textures, models or other
extracted game data. `run.bat` must resolve all public sources and generated
paths from its own directory; it must never depend on an unpublished parent
repository.

The launcher source in this directory is MIT-licensed. That license applies
only to this original launcher code and grants no rights to Fate game content,
names, trademarks, or third-party software.
