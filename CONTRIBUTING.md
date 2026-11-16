# Contributing to waffle-boot

waffle-boot is not taking outside code contributions yet. Bug reports are welcome as issues. This page says
what applies to an issue today, and to a change once contributions open.

## A clean-room project

waffle-boot is a clean-room implementation, not a decompilation. Its code is written from scratch, from
public documentation and the project's own functional specification, and the repository holds no code or
data from the device's firmware. The specification is not part of this repository.

To keep it that way, a change must be your own work, written from those sources. Never paste, attach or
link vendor firmware, or anything disassembled or decompiled from it, in an issue, a pull request, a commit
message, a comment or a test.

## A change, once contributions open

- It is released under GPL-2.0-or-later ([`LICENSE`](LICENSE)); sending it means you agree. Every source
  file starts with `/* SPDX-License-Identifier: GPL-2.0-or-later */`.
- Vendored code keeps its own license and is listed, with every local change, in
  [`src/third_party/README.md`](src/third_party/README.md).
- C11, freestanding, no warnings. US English. One focused commit per change, with a message that says why.
- Run the checks the change touches ([Building and Testing](https://github.com/bagofwaffles/waffle-boot/wiki/Building-and-Testing)).
- `src/front/` is frozen: see [Changing it](https://github.com/bagofwaffles/waffle-boot/wiki/The-Front#changing-it) before touching it.
