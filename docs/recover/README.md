# The recovery page: developer notes

A static page that does what `tools/wbrecover.py` does, from a browser with Web Serial.
[`docs/recovery.md`](../recovery.md) is the reference for recovery itself.

- `core.js`: the protocol and the decisions: framing, the link, HELLO and INFO, the image checks, a
  plan for each command, and carrying a plan out. No DOM and no Web Serial; its exported API is
  listed at its top.
- `words.js`: the protocol's numbers and every sentence, as the Python tool says them. `core.js`
  exports its names again.
- `index.html`, `app.js`, `style.css`, `serial.js`: the page, and the Web Serial transport `core.js`
  talks through.
- `install.js`, `install_words.js`: the Install tab's protocol and words, in the same style (below).
- `sim.js`: a simulated unit in place of Web Serial, from localhost only (below).
- `assets/`: the Bag of Waffles logo and background.

There is no build step and nothing to install: plain ES modules, served as they are. `core.js`
needs `crypto.subtle`, so the page must be served over HTTPS or from `localhost`.

## How it is used

1. `checkImage(bytes)` for a file; `connect(link)` for HELLO and INFO; `describe(info)` to show them.
2. `planWrite`, `planSelect`, `planAct` or `planRestart` gives a plan: allowed or refused and why,
   every warning to show, and the phrase to type. Nothing is sent.
3. `runWrite`, `runSelect`, `runAct` or `runRestart` carries a confirmed plan out. It asks the unit
   again first, and stops with nothing sent when the plan it would make now is not the one that was
   confirmed. A write's last warning belongs to the unit's answer to BEGIN: when that is not, word
   for word, what was confirmed, the run calls `hooks.confirm` before any image byte goes out.

Stopping a run stops the page only. Once END or ACT has gone out, the unit's countdown is stopped
by switching the unit off or unplugging USB, and by nothing else.

## The Install tab

The installer, `waffle-install.ldr`, runs as an OS under whatever bootloader the unit has: it is
written from that bootloader's developer menu into the boot partition the OS is not in. It runs the
hardware checks, and only when every check passes does it offer the install: a trial write, a
trial start, then a commit, after which the OS is made active again. Each write is started and
confirmed on the unit by holding the step's key for 5 s. The page guides and shows; it never sends
the bootloader image and never asks for a write.

The installer speaks recovery's framing over its USB serial port, with request types of its own:
`0x40` IHELLO (what it carries: version, length, CRC-32, front pin), `0x41` STATE (phase, verdict,
lock reason, slots, the hold's progress, each test's state), `0x42` TEST (one test's id, name and
detail) and `0x43` REPORT (`check.txt`, in chunks). Why that framing:

- the page's codec and its tests exist already (`core.js`'s `frame`, `takeFrame` and `Link`), and
  the CRC-32 and the resync rules are the same;
- the types `0x40` to `0x43` are not recovery's `0x01` to `0x07`, so neither side mistakes the
  other: recovery answers `0x40` with its unknown-request status, and the installer answers
  recovery's HELLO with `[0xFF]`. The page sends IHELLO first and falls back to HELLO;
- it is read-only, because every write is confirmed on the unit: nothing the page sends can change
  the unit.

`install.js` holds the requests, a parser for each reply, `poll` (STATE, then TEST for each test
whose state changed) and `describeInstall`, which gives the lines and advice the page shows for
each phase. The page polls about every 500 ms. A request with no answer while the port stays open
is the unit busy (it does not serve the port while it writes, and in some long checks), and the
page keeps waiting. A port that goes away after the trial write is the unit switched off for its
trial start: the page connects again on its own when the installer is back. After the end, the
unit starts its OS and no installer comes back.

For a unit already on waffle-boot with developer mode on, the tab can also send the installer file
itself: over the boot menu's developer link (the same USB vendor and product), as
`tools/testos_mon.py dev-put` does it: COBS frames with a CRC-8 and id `0x7D`, `PUT` open, the file in
240-byte data requests and close, then `SHA`, whose SHA-256 and size the page holds against the file's.
There is no retry: a data request sent twice would be stored twice. The person then writes the file
with 5 in the developer menu; the page says not to when the copy does not match. `install.js` holds
the codec and `devPut`; `tests/web/devlink_model.py` simulates the link, and the tests compare the
page's bytes with what `testos_mon.py` itself sends to it, case by case.

`FERRET_URL` in `install_words.js` is the link for a unit that has never had developer mode on;
while it is empty the page names the tool without a link.

`tests/web/test_install.mjs` checks the parsers against frames built by hand from the protocol's
tables, and walks each case of the simulated installer (`installer_model.py`, over
`unit_bridge.py`) from its first check to its end: the phases in order and the page's lines.

## Keeping it the same as the Python tool

`core.js` is a port of `tools/wbrecover.py`, and the two must decide and say the same thing. A change
to one is made in the other in the same commit. `tests/web/run.sh` checks it (Node 20 or later,
`python3`, and a C compiler for the cases on the front's own code):

- `gen_cases.py` runs the Python tool through a table of scenarios against the simulated units of
  `tests/tools/test_wbrecover.py`: its `Model` at front versions 5, 6, 9, 11 and 14, and the front's
  own protocol, slot and image code built as a shared library.
- `test_core.mjs` holds `core.js` to that: the tables and constants, the image checks, INFO's
  parsing and `describe`, each plan against the tool's decision on the same INFO, and each run,
  over `unit_bridge.py`'s socket, against the tool's run: every line, every phrase, the result and
  the flash afterwards.

A new behavior gets a row in `gen_cases.py`; the comparison then covers it.

What differs from the Python tool, on purpose:

- A page has no checkout, so it cannot tell a signed front pin from git. `planWrite` takes
  `knownPins` for that, and says "unknown" without it.
- The tool asks at its prompts; `core.js` returns the warnings and phrases in the plan, and the
  page asks.
- `core.js` sends HELLO and INFO once more at the start of a run, to see that the plan still holds.
- A DATA reply that does not move the offset on ends the run; the tool would send the chunk again.

## Trying the page with no unit

`tests/web/sim_server.py` serves `docs/` from localhost with a simulated unit behind the page:

    python3 -I -B tests/web/sim_server.py 8767
    # then open http://localhost:8767/recover/?sim=shim:plain

`?sim=KIND:CASE` picks the unit: `model` (the Python tests' model, front version 14) or `shim` (the
front's own protocol, slot and image code, built for the host), and a case from the script's `CASES`
(`plain`, `same`, `other-bad`, `unsettled`). The page then shows a button that loads an image the
simulated unit takes. `sim.js` does this only when the page is opened from localhost with `?sim`.

`?sim=installer:CASE` opens the Install tab on a simulated installer: `pass` (checks, trial,
restart, commit, done), `check-fail` (a check fails: locked), `trial-fail` (the trial start does not
reach the boot menu), `no-os` (no OS to make active again: lock reason 5), `write-fail` (the trial
write fails), `breadcrumb` (the commit is refused) and `keys-ended` (the key check ended with 8: lock
reason 6). It moves on its own timer, keeps its state
across connections as a unit does across a restart, and the page's Simulator buttons stand for
what a person does on the unit (`/sim/act?unit=installer:CASE&do=hold|leave|release|restart|reset`).

`?sim=devlink:CASE` opens the Install tab with a simulated boot menu's developer link behind the
send of the installer file: `good`, `mismatch` (the unit's copy differs), `drop` (a data request
gets no answer) or `dev-off` (developer mode off: refused). A button loads a sample file.

## Hosting

The page is static. GitHub Pages serves it from this repository's `docs/` folder (Settings, Pages,
"Deploy from a branch", `main`, `/docs`), at `/recover/` under the site's address. It loads the device
screenshots from `docs/ui/`, so the two folders stay together. Web Serial needs Chrome or Edge on a
computer, and a page served over HTTPS, which Pages is.

