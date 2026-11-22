# The recovery page: developer notes

A static page that does what `tools/wbrecover.py` does, from a browser with Web Serial.
The wiki's [Recovery Reference](https://github.com/bagofwaffles/waffle-boot/wiki/Recovery-Reference) is the reference for recovery itself.

- `core.js`: the protocol and the decisions: framing, the link, HELLO and INFO, the image checks, a
  plan for each command, and carrying a plan out. No DOM and no Web Serial; its exported API is
  listed at its top.
- `words.js`: the protocol's numbers and every sentence, as the Python tool says them. `core.js`
  exports its names again.
- `../index.html`, `app.js`, `style.css`, `serial.js`: the page, and the Web Serial transport
  `core.js` talks through. `index.html` here sends the page's old address to the site's front.
- `install.js`, `install_words.js`: the Install tab's protocol and words, in the same style (below).
- `mtp.js`, `usb.js`: the Install tab's send of the installer file over MTP (below): `mtp.js` is a port
  of `tools/mtpcp.py`'s `put` and `verify`, with no DOM and no WebUSB; `usb.js` is its WebUSB transport
  and names each way of failing to open the unit.
- `sim.js`: a simulated unit in place of Web Serial or WebUSB, from localhost only (below).
- `assets/`: the Bag of Waffles logo and background.
- `release/`: the latest release's `waffle-install.ldr` and `waffle-boot.ldr`, and `latest.json` (the
  version, and each file's size and SHA-256), put there by `tools/web_release.py`. The Install tab's
  download link points here; the Guided tab loads the bootloader image from here by default, since a
  browser may not read a release's files from GitHub, and holds it against `latest.json`.

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
trial start, then a commit. The OS has to be installed again afterward. Each write is started and
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

The Install tab is for a unit that is not on waffle-boot; one that is uses Upgrade / Recover.

### Sending the installer file

Step 1 can copy `waffle-install.ldr` to the unit itself, with WebUSB, while the unit is in its developer
menu's MTP mode. The file is the hosted release, held against `latest.json`'s size and SHA-256 first,
or the person's own file, sent as it is (its SHA-256 goes to the log). It
goes onto the unit as `INSTALL_NAME` (`os.ldr`), with the safety rules of `tools/mtpcp.py`'s `put`: the
old copy is deleted, the file sent, then read back whole in a new session and compared byte for byte.
Nothing is changed on a read-only store, and a folder of that name is never deleted. The person then
writes it as the OS on the unit, as before; the page never starts that write.

It works only where the browser can claim the unit's MTP interface. On Windows the system's own MTP
driver normally holds it, and on a Mac a photo or file-transfer program may; the page then says so,
and the download is the way. The log shows the raw WebUSB error and the interfaces the device offers.
`tests/web/test_mtp.mjs` runs each put both here and in `tools/mtpcp.py` against the responders of
`tests/tools/test_mtpcp.py` (`mtp_bridge.py`) and holds every bulk transfer to be the same bytes.

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
    # then open http://localhost:8767/?sim=shim:plain

`?sim=KIND:CASE` picks the unit: `model` (the Python tests' model, front version 14) or `shim` (the
front's own protocol, slot and image code, built for the host), and a case from the script's `CASES`
(`plain`, `same`, `other-bad`, `unsettled`). The page then shows a button that loads an image the
simulated unit takes. `sim.js` does this only when the page is opened from localhost with `?sim`.

`?sim=installer:CASE` opens the Install tab on a simulated installer: `pass` (checks, trial,
restart, commit, done), `check-fail` (a check fails: locked), `trial-fail` (the trial start does not
reach the boot menu), `no-os` (no OS to make active again: the install is offered, with no
leaving), `write-fail` (the trial write fails), `breadcrumb` (the commit is refused) and `keys-ended` (the key check ended with 8: lock
reason 6). It moves on its own timer, keeps its state
across connections as a unit does across a restart, and the page's Simulator buttons stand for
what a person does on the unit (`/sim/act?unit=installer:CASE&do=hold|leave|release|restart|reset`).

`?sim=mtp:CASE` opens the Install tab's send on a simulated MTP mode (`test_mtpcp.py`'s responders):
`good`, `readonly`, `mismatch` (a byte flipped on the write), `refused` (SendObject refused), `stale`
(a stale data container), `gone` (the unit goes away mid-copy) and `busy` (the interface is held).

## Hosting

The page is static. GitHub Pages serves this repository's `docs/` folder (Settings, Pages, "Deploy
from a branch", `main`, `/docs`); the page is `docs/index.html`, at the site's front address,
https://bagofwaffles.github.io/waffle-boot/, and loads its scripts and style from `web/`.
`?mode=install`, or `?mode=upgrade` or `?mode=recover`, opens the page with that choice made.
`docs/.nojekyll` has Pages serve the files as they are. Web Serial needs Chrome or Edge on a
computer, and a page served over HTTPS, which Pages is.

