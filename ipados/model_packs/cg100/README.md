# CG100 model pack — intentionally absent on this host

No authentic CG100 model-pack data (`board.svg`, `interface.png`, `config.json`,
`rom.bin`, `flash.bin`) exists in `/mnt/code/worktrees`, `/mnt/code/extracted` or
this port tree. The only CG100 artefacts available on the Pi are the external
native plugin binaries (`CG100SH4.plugin`, `libcig100core.*`) and the ROM
extractor tool; neither is a model pack and neither is loadable on iPadOS.

This directory is a placeholder so the bundle layout is explicit. Add the real
folder on the Mac build host as described in `../README.md`. Do not fabricate
board art, ROM or flash images to make this directory non-empty.
