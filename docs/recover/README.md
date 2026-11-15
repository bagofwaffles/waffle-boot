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

## Hosting

The page is static. GitHub Pages serves it from this repository's `docs/` folder (Settings, Pages,
"Deploy from a branch", `main`, `/docs`), at `/recover/` under the site's address. It loads the device
screenshots from `docs/ui/`, so the two folders stay together. Web Serial needs Chrome or Edge on a
computer, and a page served over HTTPS, which Pages is.

