How the boot menu's screens look and read, so that a new screen or line fits
with the rest. The lines themselves are in `src/s15_lines.c` and
`src/s15_menu.c`. Everything here may change except the rules under
[Menus and keys](#menus-and-keys), the gray formula (S-15.173) and the
hand-over frame (S-15.132). The pictures in `docs/ui/` are drawn by the
bootloader's own code (`tools/ui-shots.sh`); see [The screens](#the-screens).

## The frame

Every screen is one frame on the 480 × 222 panel (S-13c); x counts from the
left, y from the top.

| Part | Where | What |
| --- | --- | --- |
| Title bar | y 0–23, solid from side to side, four squares at x 392–471; text at y 4 | the active screen's name, top left |
| Body | 9 rows, y 33 + 19 × row, x 16–463 | the screen's lines, 56 columns |
| Status bar | solid y 202–221, text at y 205 | the status words left, `waffle-boot v<version>` right |

- **Title bar.** Solid amber from side to side with the screen's name in
  black at x 8 in title case (`Main Menu`, `Device Info`), and over it the
  boot logo's group of four 14 × 14 squares, 8 apart, at y 5–18 from
  x 392, the last ending 8 pixels from the right edge. The squares are in
  the bar's dark color: the first filled, the other three two-pixel
  outlines. The bar's text keeps left of x 383. No line under the bar.
  Only the active screen is named, with no path. With protected writes on,
  the bar is red, the squares dark red, the name white, and
  `Protected writes on` stands in white ending at x 374. A fatal error
  turns the bar red with the name `Fatal Error`. A title of up to 17
  characters fits beside the indicator.
- **Body.** Nine console rows, 9 pixels below the bar. A tenth line
  pushes the oldest off the top. Each row has a line style (below); all
  text lies on the 8-pixel grid from x 16.
- **Status bar.** No rule: dark gray (`2124`, `212421`) from y 202 to
  the bottom edge, with the status words (`SLOT1 USB`) from x 8, two
  spaces apart, and `waffle-boot v0.9.13` in gray ending at x 471. No key
  hints.

| Status word | Shows |
| --- | --- |
| `SLOT1`, `SLOT2` | the slot that is running, on every screen, in gray. A bare dark gray `SLOT` when the running slot is not established |
| `USB` | dark gray; amber while the device offers the host a disk (the upgrade screen's RAM disk, or MTP) |

Device Info spells out the second processor's state, which has no status word. Its `1 More`
opens a second page, `Device Info 2`, left with `8 Back to Device Info`.

### Bar colors

The bars are flat, with no texture. The constants are in
`src/include/waffle/s15_console.h` (`S15_MARK_*` for the squares).

| Bar | Fill | Squares |
| --- | --- | --- |
| title, amber | `FD84` (amber) | `C423` (dark amber, `C08418`) |
| title, red | `FA88` (red) | `C1E6` (dark red, `C03C30`) |
| status | `2124` (`212421`) | none |

Recovery's screens are drawn by the front (`src/front/`), which holds its
own copy of the red title bar and the status bar; a unit test keeps the
copy equal to these, pixel for pixel. Its body rows are the frame's, and
its key digits are key badges. A slot line is drawn in parts: the slot's
name and, for `WB` and a known `TE` build, its version (`v0.9.36`) in the
title bar's font; the bootloader type (`WB`, `TE`, `??`) as a chip filled
dark gray with light text; then the state chips, outlined in the color of
their meaning (`checked` green, `check failed` red, `tried`, `running` and
`being written` amber, `boot slot` gray). While an image arrives, a bar
under `Receiving n of m KiB` fills with it, and while it is written, one
under `Writing slot N: keep the power on`. On every way into recovery,
`Loading recovery menu...` shows under the logo until its first frame.

## Font

Spleen 8 × 16 (`src/third_party/spleen`, BSD-2-Clause) for all text, the
bars included: 8 pixels wide, 16 tall, 12 above the baseline and 4 below.
`tools/bdf2c.py` generates `src/display_font.c` (glyphs 0x20–0x7E) from
the BDF file. Do not shrink text to fit: reword, or split the screen.

The title bar's text is a larger bold cut, 10 × 17 with capitals 14 tall,
which `tools/bdf2c.py` derives from Spleen. Its tall symbols, such as `(`
and `|`, lose their outer rows: keep them out of titles.

## Colors

RGB565, as the panel shows them.

| Name | sRGB | RGB565 | Used for |
| --- | --- | --- | --- |
| black | `000000` | `0000` | the background |
| text | `E8E8E8` | `EF5D` | lines, labels, values |
| dim | `8C8C8C` | `8C71` | hints, keys of key–value rows, the version |
| faint | `5A5A5A` | `5ACB` | the `v0.9.13` of the version |
| unlit | `484848` | `4A49` | status words that do not apply |
| rule | `383838` | `39C7` | the key test's outlines |
| amber | `FFB020` | `FD84` | the title bar, key badges, key names, warnings |
| red | `FF5040` | `FA88` | protected writes on, an item held to confirm it, errors |
| green | `50D870` | `56CE` | done, installed |

Black text on amber and white text on red are the only text on a filled
background, with a key badge's black digit; the status bar's text sits on
its dark gray.

### Key badges

Every key digit on a screen is a key badge: a solid amber cell 16 × 16,
centered on the digit's column (from 4 pixels left of it and a pixel above
the row's top), its four corner pixels left out, with the digit in bold black
(its glyph drawn twice, one pixel apart). The cell covers half of each column
beside the digit, so a badge always has a blank column, or a tab, on each side. No border and no second shade. On an `armed` row the badges
are red. With the color off they draw as the grays below.

### Grayscale

Display's `3 Color` (Settings, 3) turns color off (S-15.173); with no saved setting it is off on the OP-XY and on on the
OP-1F. Every pixel is then drawn in the gray of its color's luma,
`Y = (77 R + 150 G + 29 B + 128) / 256` on the 8-bit components. The grays
above (text, dim, faint, unlit, rule, the status bar) stay as they are.
Amber becomes `BDD7` (BDBDBD), red `8430` (848484), green `A534`
(A5A5A5), dark amber `5AEB` (5A5D5A) and dark red `39E7` (393C39).
Lines, layout and keys do not change. The setting is kept in
the running slot's settings sector with the others (S-15.172). The installer,
which reads no saved setting, draws as the device's default: in gray on the OP-XY, in color on the OP-1F;
its display check's strip of colors, which tests that the panel shows them, is in color on both.

## Line styles

`tests/vectors/s15/ui/` writes a line as `style:text`; a line with no style is
`text`. In the text, a tab (`\t`) separates the parts of a line, and a
backquote (`` ` ``) starts and ends an amber span in a `keys` line, an
item's hint and a step's label; a span of one digit is a key badge. Columns count from the
body's left edge (x 16).

| Style | Text | Drawn as |
| --- | --- | --- |
| `text` | anything | white |
| `dim` | anything | gray |
| `head` | a heading or question | amber |
| `keys` | a sentence naming keys | gray, the `` `spans` `` amber, a digit's span a key badge |
| `item` | `D Label` or `D Label\tHint` | the digit's key badge in column 0, the label white from column 3, the hint gray, right-aligned |
| `item-on` | as `item` | the hint red (a setting that is on and needs care) |
| `armed` | as `item` | the whole row red, its badges too: an item whose key is being held to confirm it |
| `back` | `8 Back to <Screen>` | the digit's key badge, the label gray |
| `pick` | `D Label\tDetail\tNo` | indented: the state gray in column 3, left of the digit's key badge in column 8, the label white from column 11, the detail white from column 27 |
| `pick-on` | as `pick`, `Yes` | the state black on a solid red block, so that it stands apart from `No` in gray too |
| `pick-off` | `D Label\tWhy` | indented, all gray, no state: the digit plain in column 8, the label from column 11, why it cannot be picked from column 27 |
| `ok` | a sentence | a green `+` in column 0, the sentence green from column 3 |
| `warn` | a sentence | an amber `!` in column 0, the sentence amber from column 3 |
| `error` | a sentence | a red `!` in column 0, the sentence red from column 3 |
| `kv` | `Key\tValue` or `Key\tValue\tKey\tValue` | keys gray (columns 0 and 32), values white (columns 14 and 43) |
| `step-run` | `Label\tStatus` or `Label\t<p>%\tText` | an amber `>`, the label white from column 3; a status `<p>%` draws a progress bar from column 19 with `<p>%` after it, or the third part (`Text`) in its place; any other status amber |
| `step-ok` | `Label\tStatus` | a green `+`, the label white, the status green from column 19 |
| `step-skip` | `Label\tStatus` | a gray `-`, the label white, the status gray from column 19 |
| `step-fail` | `Label\tStatus` | a red `x`, the label white, the status red from column 19 |

A tab means something only in the styles with parts (`item`, `item-on`,
`armed`, `back`, `kv`, `step-*`, `pick*`); elsewhere it shows as `?`. The log
gets the text with the backquotes of a `keys` line and of the styles with
parts removed, and each separating tab written as two spaces.

## Writing

- **Spelling.** US English: `Color`, `gray`.
- **Capitals.** Screen names in the title bar are title case
  (`Content Packs`). Everything else is sentence case: the first word and
  proper names only (`Remove content packs`, `Factory reset`).
  Abbreviations stay in capitals (DSP, BLE, USB, MTP, OS, TE, OP-XY);
  `waffle-boot` is always written so.
- **Key names** are lower case inside a sentence and set in amber, a
  digit as a key badge: `` `1` Save (hold to select)   `8` Cancel ``.
- **Length.** A line is at most 56 characters; an item's label and hint
  together at most 51. Shorter is better: say one thing per line.
- **Punctuation.** Items, hints and headings carry no full stop; sentences
  do. `!` only in `Done!`.
- **Values** (versions, serials, pack names) are shown as they are, never
  re-cased.

## Menus and keys

These rules are fixed: only the maintainer changes them. A new item, screen or setting that follows
them and their exclusions needs no sign-off.

### Rules

- Items are numbered with the digit that chooses them (S-15.011), in digit order. Gaps are fine, and a
  digit keeps its meaning wherever it is shown.
- Digit 8 leaves a screen: every screen below the main screen that a key leaves is left with 8, as
  `8 Back to <the screen it returns to>`, or `8 Cancel` where it declines what the screen asks. No screen
  is left with "any key". A screen with something under way goes back only once nothing is (the upgrade
  screen, S-15.103).
- Whatever removes, erases, formats or overwrites, and turning Protected writes on, is confirmed by one
  rule: the item's own key held for 5 s. The items it confirms carry one hint, `Hold to select`, on a
  `dim` line under them. Its key down starts the hold: its row turns `armed`, with no hint, and the last
  row is a bar that fills, with the seconds left (`Release to cancel`). Held the whole 5 s, the action
  runs once, and the key's release does nothing more. Released earlier, or any other key down, only
  cancels and draws the screen as it was. A knob click never starts a hold, and the white knob confirms
  nothing. Where the item names its hold in its label as `(hold to select)` (`1 Install selected (hold to
  select)`, S-15.116, `2 Turn on (hold to select)`, `1 Allow (hold to select)`), it has no hint and no
  line under it, armed or not.
- A factory reset is always confirmed, from every menu that offers it.
- Keys pressed before the boot menu's first screen, while `Loading boot menu...` shows among them, do
  nothing, and a key still down when it opens acts only after it has been let go and pressed again
  (S-15.013).
- The result of a removal or a reset is a view of its own, left with 8 alone.
- A setting shows its state as the item's hint (`Off`, or `On`, in red where it needs care).
- Keys that are not items are named in a `keys` line on the screen that reads them, not in the status
  bar.

### Exclusions

- The main screen holds no destructive item and no setting.
- No setting sits on a digit the white knob also gives.
- 8 is never an item.
- Developer mode is set only by its item on Settings.
- Protected writes is set only on Settings, and never from a host.
- A host never starts a factory reset.

## Current layout

A description of the screens as they are now, free to change within the rules above.

| Screen | Items |
| --- | --- |
| Main Menu | 1 Install firmware, 2 Settings, 3 Developer menu (while developer mode is on), 4 and the help key Device Info, 5 System; then 7 Continue to OS at the bottom, set apart by a blank row or by the failed-start notice. 6 is no item |
| Settings | 1 Developer mode, 2 Protected writes, 3 Display, 5 MTP target (with mtp-two-items), 6 Start in boot menu, 7 Host link |
| Display | 3 Color, 6 Boot logo (its state as the hint: `Built-in`, `Custom`, `No room`, `Unavailable`; the last two open nothing; no item without the logo's store, as in the test OS), 8 Back to Settings; Color is saved with Settings' items |
| Protected Writes | 2 Turn on (hold to select), 8 Back to Settings; off again after every restart; while it is on, Settings' 2 turns it off at once |
| System | 1 Key test, 2 Reset volume calibration, 3 Remove content packs (when there are packs), 5 Format drive, 7 Factory reset (6 is no item); the format is shown on every unit, whatever developer mode is |
| Boot Logo | 5 Remove (`Hold to select`, while one is stored), 8 Back to Display; the disk offered, a picture dropped and ejected opens its view: 1 Save (hold to select) saves it, 8 cancels; the result is a view left with 8 alone |
| Developer | 2 MTP file transfer (one item, its interface the MTP target's; 8 turns MTP off), 3 Start /os.ldr once (does not write), 5 Write /os.ldr to boot partition, 6 Restart; it offers no reset |
| Host Link | 1 Allow (hold to select), 8 Cancel; 60 s with no key cancels; the unit's own keys alone |
| Backplane Link, Backplane Reset | A computer's PSoC request: 8 Cancel during its 15 s countdown, the unit's own key alone, no hold (the keys come from the PSoC); then, for the link, a screen with no key while the computer has it |
| Key Test | Shift + 8 leaves it |
| Recovery's slot menu (its main screen's 1) | 1 Try slot N once, 2 Make slot N the boot slot, 3 Keep slot N, 4 Put slot N's record back, each only where it applies and each confirmed by key 1 held for 5 s on the screen after it; while 3 is shown, 2 is not; 8 goes back |

## Behavior

- Start in boot menu is off by default. On, every start that would boot the OS opens the boot menu
  instead (S-15.175), but the one start after Continue to the OS or after the developer menu's start
  of `/os.ldr` once. It is kept with the other settings in the running slot's settings sector in the SPI
  flash; when it cannot be saved, Settings says so and the item shows what the next start will read.
- Continue to the OS restarts the unit, and that one start boots the OS even with Start in boot menu on
  (S-15.190). The main screen's 7, the 6 Restart offered after an install that completed, after a
  canceled selection and after an OS write that succeeded, and a host's request (`continue-os`,
  S-10c.076) start it; elsewhere on the developer menu 6 is the plain restart, which repeats this boot's
  reason. It opens no screen of its own: a line under the screen it was chosen on says that the unit
  is restarting to start the OS, and from then no key acts.
- Start `/os.ldr` once restarts the unit, and that one start boots the OS from the drive's `/os.ldr`
  even with Start in boot menu on (S-15.194, S-04.014); it writes nothing to the eMMC. Like Continue
  to the OS it opens no screen of its own: a line under the screen it was chosen on (for a host's
  request, the screen showing) says that the unit is restarting to start it, and from then no key acts.
- The OS write has its own screen: its steps and their progress, and on a failure the reason and what
  the eMMC was left with. 8 goes back to where it began (the developer menu, or for a host's write the
  screen it began on); after a success 6 restarts into the OS.
- The upgrade screen's disk form has a back line, and no key there sets Protected writes or starts a
  scan: a scan starts only when the host releases the disk. A host install after reason 40, whose screen
  has no way back, runs with Protected writes off.
- Developer mode is off by default. It shows the developer menu and adds the USB serial port.
- The color setting is off by default on the OP-XY and on by default on the OP-1F; off draws in gray (S-15.173).
- The settings are kept per slot in the SPI flash, each at its default when the slot's settings sector
  holds no valid record for the image it runs (S-15.172).
- The main screen shows the failed-start notice only when this boot's start of the OS failed (S-15.163):
  under items 1 to 5 and above 7, `The OS could not be started.` and a `   Reason: …` line; for an image
  marked encrypted the OS-reinstall line is its reason (S-15.161). Device Info then gives the failed step
  on its `OS start` row, in place of the blank row above `8 Back to Main Menu`.

## The key test

One box per key on a grid of the device's front (17 columns, 6 rows, the four
knob clicks 2 × 2), with faint outlines where the speaker, the volume knob and
the display are. A box is dark gray (`4A49` outline, `1082` inside) until its
key is confirmed, amber while the key is held (`FE06` outline, `ED04` inside),
and light gray once confirmed (`EF5D` outline, `BDD7` inside).

## The hand-over frame

The one frame shown while the OS loads, until the OS draws its own
(S-15.132). It has no title bar, rule or status words. Sixteen 14 × 14
step squares stand in four groups of four across the middle (x 59–420,
y 104–117): the first of each group white, the others a two-pixel faint
outline. It carries no text: the name and version show only in the boot
menu's and the recovery menu's bars.

<img src="../docs/ui/handover.png" width="480" alt="Hand-over frame">

A boot logo stored on the unit ([Boot Logo](Boot-Logo)) takes the frame's
place, as the front drew it at that start: the picture centered on its
background, nothing over it. Its view on the unit, before a save, is the
same picture with one line over its last rows: the sizes and the keys, or
the hold's time left.

A build can show its own picture instead: `make image LOGO=<file>.png`,
with a PNG of exactly 480 × 222 pixels, shown alone with nothing drawn
over it. Colors convert as in the table above; translucent pixels blend
onto black. The picture is stored as runs of one color, so flat areas
cost little, while gradients, photos and dithering may not fit
(`tools/png2logo.py` refuses one over 160 KiB).

## The screens

Rendered by `tools/ui-shots.sh` from `tools/ui_shots.c`, at the
panel's own size, with the color off. Two things are in color: Settings with
the color on, as an example, and recovery, which the setting does not
reach. `tour.gif` shows a few of them in turn, for the README. They show the
tree's version (recovery's other slot one patch older, an offered upgrade one
newer), and each PNG carries it: after `make bump-version`, redraw them, or
`make check` fails (`tools/ui_shots.py --check`).

| | |
| --- | --- |
| <img src="../docs/ui/main.png" width="480" alt="Main Menu"> | <img src="../docs/ui/main-states.png" width="480" alt="Main Menu with every state"> |
| Main Menu | developer mode on, the second processor absent, the OS to reinstall, protected writes on |
| <img src="../docs/ui/settings.png" width="480" alt="Settings"> | <img src="../docs/ui/settings-color.png" width="480" alt="Settings, color on"> |
| Settings | Settings with the color on |
| <img src="../docs/ui/option.png" width="480" alt="Protected Writes"> | <img src="../docs/ui/option-held.png" width="480" alt="Protected Writes, 2 held"> |
| Protected Writes, from Settings' 2 while it is off | its 2 held for 2 of the 5 s |
| <img src="../docs/ui/system.png" width="480" alt="System"> | <img src="../docs/ui/system-armed.png" width="480" alt="Factory reset held"> |
| System, with content packs | the factory reset, 7 held for 2 of the 5 s |
| <img src="../docs/ui/info.png" width="480" alt="Device Info"> | <img src="../docs/ui/keytest.png" width="480" alt="Key Test"> |
| Device Info | Key Test, one key down, one tested |
| <img src="../docs/ui/packs.png" width="480" alt="Content Packs"> | <img src="../docs/ui/developer.png" width="480" alt="Developer"> |
| Content Packs, pack 1's removal held for 2 of the 5 s | Developer |
| <img src="../docs/ui/format.png" width="480" alt="Format Drive"> | <img src="../docs/ui/upgrade-wait.png" width="480" alt="Install Firmware"> |
| Format Drive | Install Firmware, waiting for a package |
| <img src="../docs/ui/upgrade-run.png" width="480" alt="Installing"> | <img src="../docs/ui/upgrade-done.png" width="480" alt="Done"> |
| a run in progress | a run done: the BLE installed, its bootloader already installed, the backplane left at `No` |
| <img src="../docs/ui/upgrade-failed.png" width="480" alt="Failed"> | <img src="../docs/ui/upgrade-select.png" width="480" alt="The parts of a package"> |
| a run failed | the parts of a package, as the list opens: every row at `No` |
| <img src="../docs/ui/upgrade-picked.png" width="480" alt="Parts chosen"> | <img src="../docs/ui/upgrade-held.png" width="480" alt="Install selected held"> |
| two parts at `Yes`, the bootloader among them; a row that cannot be picked, up to date | 1 held for 2 of the 5 s |
| <img src="../docs/ui/approve.png" width="480" alt="Host Link"> | <img src="../docs/ui/approve-held.png" width="480" alt="Host Link, 1 held"> |
| Host Link: a computer asks for the session (APPROVE) | its 1 held for 2 of the 5 s |
| <img src="../docs/ui/main-failed.png" width="480" alt="The OS not started"> | <img src="../docs/ui/info-failed.png" width="480" alt="Device Info after it"> |
| Main Menu: the start failed when the second processor stopped answering | Device Info: the step, the request and where it broke |
| <img src="../docs/ui/system-lost.png" width="480" alt="System, the slave lost"> | <img src="../docs/ui/upgrade-canceled.png" width="480" alt="Nothing installed"> |
| System while the second processor does not answer | the selection canceled: nothing installed, 6 restarts, 8 goes back |
| <img src="../docs/ui/upgrade-sig-prompt.png" width="480" alt="The signature does not match"> | <img src="../docs/ui/upgrade-signed.png" width="480" alt="A valid package's parts"> |
| a package whose signature does not match: 1 held goes on to its parts, 8 cancels | a package with a valid signature: `Signed by TE` under the list |
| <img src="../docs/ui/oswrite-run.png" width="480" alt="Write OS"> | <img src="../docs/ui/continue-restarting.png" width="480" alt="Restarting into the OS"> |
| Write OS (developer menu, 5), the write running | Continue to the OS: the restarting line under the screen it was chosen on |
| <img src="../docs/ui/oswrite-done.png" width="480" alt="Write OS done"> | <img src="../docs/ui/oswrite-failed.png" width="480" alt="Write OS failed"> |
| the OS written, 8 back to the developer menu | the OS write failed: the step, the reason, what the eMMC was left with |
| <img src="../docs/ui/info2.png" width="480" alt="Device Info 2"> | <img src="../docs/ui/recovery-loading.png" width="480" alt="Loading recovery menu..."> |
| Device Info 2, from Device Info's 1: the start log | the line under the logo on the way into recovery |
| <img src="../docs/ui/recovery.png" width="480" alt="Recovery"> | <img src="../docs/ui/recovery-slots.png" width="480" alt="Recovery: slots"> |
| Recovery, connected | the slot menu (1, `Change the boot slot (optional)`) |
| <img src="../docs/ui/recovery-confirm.png" width="480" alt="Recovery: confirm"> | <img src="../docs/ui/recovery-countdown.png" width="480" alt="The countdown"> |
| a try confirmed with 1 held for 3 of the 5 s | the countdown before a write from a computer |
| <img src="../docs/ui/recovery-receiving.png" width="480" alt="Receiving"> | <img src="../docs/ui/recovery-writing.png" width="480" alt="Writing"> |
| an image for slot 2 arriving | slot 2 being written |
| <img src="../docs/ui/recovery-te.png" width="480" alt="A TE slot"> | <img src="../docs/ui/recovery-unknown.png" width="480" alt="An unknown slot"> |
| slot 2 holds `TE v1000.0.20` | slot 2 holds `??` |
| <img src="../docs/ui/recovery-keep.png" width="480" alt="Recovery: slots after a trial"> | <img src="../docs/ui/logo.png" width="480" alt="Boot Logo"> |
| the slot menu after slot 2's trial start came up: 3 keeps it, no 2 | Boot Logo (Display, 6) with a logo stored: the disk offered, 5 held removes it |
| <img src="../docs/ui/logo-view.png" width="480" alt="A picture before its save"> | <img src="../docs/ui/display.png" width="480" alt="Display"> |
| a picture dropped on the disk, as the start will show it: 1 held saves it, 8 cancels | Display, from Settings' 3: the color and the boot logo |

## Adding a screen

1. Give it a title-case name for the title bar.
2. Fit its lines into nine rows of 56 columns in the styles above; if they
   do not fit, split the screen or reword, never shrink the font.
3. Put its items in digit order, with `8 Back to …` last.
4. Write its lines into the code (`src/s15_menu.c`) and its test data
   (`tests/vectors/s15/ui/screens.json`), and render it with
   `tools/ui-shots.sh` to check it by eye.
