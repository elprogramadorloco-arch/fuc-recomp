Place these three user-provided files directly in this directory:

- game.iso (the legally obtained NPEH00007 version 1.01 PSP ISO);
- font.zip containing jpn0.pgf, ltn0.pgf through ltn15.pgf, and kr0.pgf;
- ppsspp.zip (official stable Windows portable ZIP).

Then run run.bat from the public root. Its TUI validates the files and the
installed build toolchain before compilation, then displays build progress.
PPSSPP is extracted from this exact ppsspp.zip and closes automatically after
the verified EBOOT dump; no menu input or manual close is required.
This directory is private and ignored by Git.
Never commit or redistribute its contents.
