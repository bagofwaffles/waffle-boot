# Working on waffle-boot

Rules for anyone changing this repository, people and coding agents alike. `CONTRIBUTING.md` has
the clean-room rule in full.

## The clean-room boundary

This does not bend. Code is written only from public documents, upstream sources, this repository and
measurements on a unit. Never read vendor firmware or decompiled material, and never ask what the vendor's
bootloader does.

## Requirement labels

Comments and tests cite requirement labels such as `S-02.084`. The specification they come from is not part
of this repository; `wiki/Specification.md` lists what each number covers. Keep a label when you move code,
and never reuse one. The test data, reference models and generators are under `tests/oracle/`: a change to
behavior updates them in the same pull request.

Screen wording, layout, colors, menu layout and log wording are free to change. The rules under "Menus and
keys" in `docs/ui-style-guide.md` are not: ask the maintainer first (for example, a factory reset always
asks for confirmation).

To reword a screen, edit the line and its entry under `tests/vectors/s15/ui/`, run `make ui-update`, read
the diff, and fix any unit test that quotes the old text.

## The frozen front

`src/front/` runs from the image's entry to the key read and holds recovery (Power + M4). It is linked
alone, and its loaded bytes are pinned in `src/front/front.pin`; every cross build fails when they change.
A front that fails takes recovery with it, so the front and its pin change only with the maintainer's
sign-off, and a changed front needs a recovery round on a unit (`docs/recovery.md`) before it is committed
to a slot.

## Flash writes

Read `docs/flash-writes.md` before changing anything that erases or programs the SPI flash. A change there
needs the maintainer's sign-off when it could leave a unit that does not start.

## Checks

`tools/ci-local.sh` runs the checks in a container (Docker); `make check` does not link on macOS. Run the
jobs a change touches: `host` and `analysis` for `src/`, `cross` when the board image changes, `repro` when
the build changes, `conformance` for `tests/oracle/` and the conformance tests.

## What goes in a change

- One pull request per finished piece of work.
- US English. Short comments that explain the code; no dates, names or history in them.
- No personal names, email addresses, home paths or host names in files or commit messages.
  `tools/leak-check.py` checks every commit (`tools/leak-check-install.sh` sets it up). Do not skip the
  hooks.
