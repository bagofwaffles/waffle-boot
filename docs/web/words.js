// SPDX-License-Identifier: GPL-2.0-or-later
//
// Recovery's numbers and words: the protocol's constants (src/front/fr_proto.h), the status and
// refusal tables, and what INFO means, said as tools/wbrecover.py says it. core.js imports this
// file and exports its names again; a page imports core.js only.
//
// Every function here is pure: it takes the INFO object of core.js's parseInfo() (the fields
// carry the Python tool's names) and returns strings or arrays of strings. The words name the
// Python tool's commands (wbrecover.py write IMAGE, --running, --force): they are the same
// actions under the names wiki/Recovery-Reference.md gives them.

export const VID = 0x2367, PID = 0x0021;
export const PRODUCT = 'OP-XY Recovery';
export const PID_1F = 0x0102, PRODUCT_1F = 'OP-1 Recovery';   // recovery on the OP-1F (S-02.093)

export const PROTO = 2;                 // from front version 11
export const OLD_PROTO = 1;             // fronts 5 to 10
export const HELLO = 1, INFO = 2, BEGIN = 3, DATA = 4, END = 5, RESTART = 6, ACT = 7, REPLY = 0x80;
export const MAX_DATA = 1024;
export const MAX_PAYLOAD = MAX_DATA + 8;
export const HEAD = 6;
export const FRAME_MAX = HEAD + MAX_PAYLOAD + 4;
export const NO_SLOT = 0xFF;
export const SLOT_CAP = 0x78000;        // S-02.002, S-02.044: what the unit takes
export const IMAGE_MAX = 0x77000;       // S-02.095: recovery's own limit, the slot's last sector holds the start log
export const WB_IMAGE_MAX = 0x76000;    // S-02.104: this tool's, the sector below it holds the slot's settings
export const BEGIN_REPLY_LEN = 3, DATA_REPLY_LEN = 5, END_REPLY_LEN = 2, ACT_REPLY_LEN = 4;
export const HELLO_LEN = 47, HELLO_O_VERSION = 7, HELLO_O_PIN = 15;
export const INFO_V5 = 199, INFO_V6 = 219, INFO_V11 = 232;
// from front 20: the device (0 OP-XY, 1 OP-1F), how it was decided (0 the first stage's table, 2 not
// identified, the OP-XY's default) and the first stage's n and c (S-18.003, S-18.004)
export const INFO_V20 = 242;
export const DEVICES = ['OP-XY', 'OP-1F'];
export const BATT_MAX = 16;
export const F_ESTABLISHED = 1, F_SELECTOR = 2, F_OWN_DESC = 4, F_MOVED = 8, F_SPOILED = 16,
    F_HELD = 32, F_READ_ERROR = 64, F_UNSETTLED = 128;
export const S_GOOD = 1, S_HAS_DESC = 2, S_HAS_FRONT = 4, S_TRIED = 8, S_STARTED = 16;
// good or tried, and S-02.106 rule 3 holds (front 17 and later)
export const S_TE_PREFIX = 32;
// S-02.106: the TE bootloader builds by (length, CRC-32 of the slot's bytes).
export const TE_BUILDS = [
    [0x5ADB0, 0x7AF4665C, [1000, 0, 12, 0]],
    [0x5B7AC, 0x0789B2D9, [1000, 0, 13, 0]],
    [0x60000, 0x5DEE4B51, [1000, 0, 20, 0]],
    [0x60CA4, 0xA930BEF3, [1000, 0, 23, 0]],
];
export const BEGIN_F_RUNNING = 1, BEGIN_F_TRIAL = 2, BEGIN_F_SELECT = 4, BEGIN_F_FORCE = 0x80;
export const WAY_NONE = 0, WAY_PLAIN = 1, WAY_SELECT_FIRST = 2, WAY_IN_PLACE = 3, WAY_TRIAL = 4,
    WAY_SELECT = 5, WAY_TRY = 6, WAY_SWITCH = 7, WAY_KEEP = 8, WAY_PUT_BACK = 9;
export const ACT_TRY = 1, ACT_SWITCH = 2, ACT_KEEP = 3, ACT_PUT_BACK = 4;
export const TIER_NONE = 0, TIER_SAME = 1, TIER_FRONT = 2, TIER_FOREIGN = 3, TIER_BAD = 4;
export const TRIAL_FRONT = 7;           // the first front that takes another front, by trial
export const KEEPS_WRITE = 8;           // the first whose refusals leave INFO showing an earlier write
export const ACTS_FRONT = 11;           // the first with the slot actions, the force flag, the countdown
export const SAME_FRONT = 14;           // the first that writes an image the kept slot already holds
export const COUNTDOWN = 15;            // seconds from an accepted END or ACT to its first erase
export const ACT_NAMES = { 1: 'try', 2: 'switch', 3: 'keep', 4: 'put-back' };
export const ACT_WAYS = { 1: WAY_TRY, 2: WAY_SWITCH, 3: WAY_KEEP, 4: WAY_PUT_BACK };
export const WHY = {
    1: 'Power + M4 was held',
    2: "the image check failed: the running slot's image is not intact",
    3: 'three starts in a row did not finish (S-02.095); the power switch leaves recovery, and ' +
        'the next start is a normal one',
};
export const PHRASE = 'erase the running slot in place';
export const TRIAL_PHRASE = 'try the new front';
export const SWITCH_PHRASE = 'make it the boot slot';
export const forcePhrase = (name) => `force the ${name}`;
export const PHASES = ['idle', 'receiving', 'writing', 'done', 'failed', 'counting down'];
export const IDLE = 0, RECEIVING = 1, WRITING = 2, DONE = 3, FAILED = 4, COUNTING = 5;

export const RESET_HINT = 'Restart once without holding a key, then hold Power + M4 again.';
export const SAFE_HINT = 'Run info first: restart only with a checked boot slot; the unit refuses ' +
    'a restart while a write of this recovery has left no checked boot slot.';
export const FRONT_HINT = 'Another front goes in only as a trial: write --new-front IMAGE, on a ' +
    'front of version 7 or later; from an older front, the test OS installer (wiki/Test-OS.md).';
export const OTHER_HINT = 'Start the other slot if it is checked and holds a stream the boot ROM can ' +
    'start (S-02.091): wbrecover.py try starts it once, wbrecover.py switch makes it the boot slot; or key 1, ' +
    '"Change the boot slot", in the recovery menu.';
export const SLOT_MENU = 'or key 1, "Change the boot slot", in the recovery menu';
export const NOT_STARTED_HINT = 'Write it again with --new-front and restart with no key held, so ' +
    'that it starts once; then switch off and on and select. Or select from its own recovery in the ' +
    'trial start (try holding key 4 through the restart). A front older than version 7 never leaves ' +
    'that mark: put it on with the test OS installer (wiki/Test-OS.md).';
const oldFrontRisk = (slot) => 'A front without the trial mark (any front older than version 7) ' +
    `can never be selected: ${slot}'s image is then lost for nothing.`;

// enum fr_write_rc, then the protocol's own codes: [what happened, what to do]
export const STATUS = {
    0: ['ok', ''],
    1: ['a write or an action is already under way, or counting down to its first erase',
        'Wait for it to end: run info.'],
    2: ['the unit could not establish which slot it is running from (S-01.002)', RESET_HINT],
    3: ['the running slot is not the slot the selector names', RESET_HINT],
    4: ['the selector is not settled: its verified byte is not 01 (S-02.090)', RESET_HINT],
    5: ['the boot slot failed its check, and is not the running slot with its front intact (S-02.084)',
        OTHER_HINT],
    6: ['the image is empty or too large: recovery takes at most 0x76000 bytes, 0x77000 before front 17 (S-02.084)',
        'Check the file: wbrecover.py check IMAGE.'],
    7: ['the image is not a bootable stream with a waffle-boot front (S-02.091)',
        "Check the file (wbrecover.py check IMAGE) and build it with this tree's tools."],
    8: ['the image arrived damaged: its SHA-256 is not the one announced', 'Try the write again.'],
    9: ['a flash erase, program or read of the write failed',
        'The selector was not changed: the unit still starts as before. Try the write again.'],
    10: ['the slot did not read back as written, or not as the SHA-256 announced',
        'The selector was not changed: the unit still starts as before. Try the write again.'],
    11: ['the slot was written and read back, but the selector write failed or did not read back',
        SAFE_HINT],
    12: ["the image's front is not the one this unit runs and no trial was asked for, or the " +
        'running slot is checked and holds no waffle-boot front (S-02.084)', FRONT_HINT],
    13: ['the slot that is kept already holds this image, so the unit could not tell which slot ' +
        'runs; or the target is the checked running slot, which already holds it (a front before ' +
        'version 14 refuses this; from version 14 both slots may hold the same image)',
        'Nothing to do; or write another build.'],
    14: ['the selector write before the write of the running slot failed; no slot or record was ' +
        'written', 'Run info to see what the selector names now.'],
    15: ['an in-place write of the running slot becomes possible only after a write of the other ' +
        'slot has failed in this recovery (S-02.084)',
        'Try a plain write first (wbrecover.py write IMAGE): it is safe and loses nothing.'],
    16: ["the unit's state changed since the write was confirmed: it would not go as BEGIN's " +
        'answer said, and nothing was written', 'Run info, then write again.'],
    17: ['the selector or a slot could not be read for the start condition; nothing was written',
        'Run info and try again.'],
    18: ['the target is the only checked slot, so the unit accepts only the image it holds, to settle ' +
        'the selector on it (S-02.084)', 'Take one of the ways out that wbrecover.py info lists.'],
    19: ['a trial needs the running slot checked, and neither a trial nor a select is taken once ' +
        'this recovery has begun a write of the running slot, written the selector to the other ' +
        'slot, or begun a trial write (S-02.084)',
        'Run info to see the state; after a trial write, restart first.'],
    20: ['no slot holds this image as tried where a select can take it: from the old front the ' +
        'other slot, in a trial start the running slot (S-02.084)',
        'Send the file that was written with --new-front, after its trial start has run.'],
    21: ['the tried slot has not started: its front has left no mark of a trial start (S-02.084)',
        NOT_STARTED_HINT],
    22: ['that slot is already the boot slot (verified 01), so nothing was written',
        'Nothing to do: restart the unit (wbrecover.py restart) to start it.'],
    23: ['the other slot failed its check and is not tried: nothing checks it at a start (S-02.084)',
        'Write an image to it first (wbrecover.py write IMAGE).'],
    24: ['the write was stopped before its first erase: the unit was switched off or USB was ' +
        'unplugged, and nothing was written',
        'Run info, then start the command again if it is still wanted.'],
    0x40: ['the unit did not understand the request',
        "Run info: the unit's front may be older or newer than this tool knows."],
    0x41: ['not now: no transfer is open, or a write is under way',
        'Run info, then start the command again.'],
    0x42: ['a chunk came out of order', 'Start the command again.'],
    0x43: ['the transfer is short, or a chunk runs past its end', 'Start the command again.'],
    0x44: ['the unit refuses a restart: a write of this recovery left the running slot damaged, or ' +
        'did not settle the selector on it, and a restart is not safe yet',
        'Write a slot first: each way out ends by settling the selector.'],
};
export const IMAGE_RC = {
    1: 'empty or larger than a slot (S-02.044)',
    2: 'its blocks do not parse up to a FINAL block (S-02.091 rule 1)',
    3: 'its INIT block is missing, repeated, or not in L1 (rule 2)',
    4: "a block loads outside L1, L2 and DDR or into the boot ROM's reserve (rule 3)",
    5: "a block loads into the first stage's stack, 0x11807000-0x11808000 (rule 4)",
    6: "a block code's BCODE is not 4 (rule 5)",
    7: 'it carries no waffle-boot front',
};
// Hints that send the user to a restart or to the other slot, or say the unit starts as before: never
// while a write of this recovery has left the running slot bad (retryLines() says what to do).
export const RESTART_TIPS = [2, 3, 4, 5, 9, 10, 11, 22];
// statuses 3 and 4 once this recovery has made a write
export const AFTER_WRITE = ['restart before another write: this recovery has made a write, and the ' +
    'unit takes the next one only after a start', 'Restart the unit (wbrecover.py restart) with no ' +
    'key held, then hold Power + M4 again.'];
export const READ_ERROR = 'A read of the selector or a slot failed, so what the unit reports of ' +
    'them may not be what the flash holds. Ask again (wbrecover.py info): the unit reads them ' +
    'again, and by itself every 5 s while it holds.';
export const COUNTDOWN_LINE = `The unit counts down ${COUNTDOWN} s before its first erase. ` +
    'Switching the unit off or unplugging USB in that time stops it, with the flash untouched; ' +
    'stopping this tool does not.';
// S-02.097: a recovery write leaves the slot's settings sector as it is; the settings there were
// saved for the image they name, so another image starts on the defaults.
export const SETTINGS_LINE = "The slot's settings start at their defaults unless it held this same image.";
export const RESTART_RISK = 'Forced, the unit restarts all the same: it may not start at all, and ' +
    'then recovery is not reachable either; there may be no way back without a flash programmer. A ' +
    'write first settles the selector (the ways out above).';
export const FORCE_HEAD = 'With --force the unit lifts the refusals that protect it:';
// What force lifts for a write (S-02.084), and what could go wrong.
export const WRITE_RISK = {
    3: 'The selector does not name the running slot. Forced, the write goes to the running slot, ' +
        'the one the selector does not name; the unit takes it only while the boot slot is ' +
        'checked, which a cut of the write then starts.',
    4: 'The selector is not settled: its verified byte is not 01, so the next start was to check ' +
        'it. Forced, the write goes on over it: a cut can leave a selector that leads to no good ' +
        'slot.',
    5: "The boot slot failed its check, and the running slot's front is not intact in the flash. Forced, " +
        'no front is asked of the image: the unit can end on an image whose recovery is not this one, ' +
        'or none.',
    12: "The running slot's front is not intact in the flash. Forced, no front is asked of the " +
        'image: the unit can end on an image whose recovery is not this one, or none.',
    15: 'The other slot failed its check and no write of it has failed here. Forced, the running slot is ' +
        'erased and written in place at once: a cut leaves a unit that cannot be repaired without a ' +
        'flash programmer. A plain write of the other slot first is safe (wbrecover.py write IMAGE).',
    19: 'The running slot is not checked. Forced, the trial goes on: if the new front does not come up, ' +
        'switching off and on starts the running slot, which may not start either.',
    front: "The file's front is not the one this unit runs. Forced, it is written and selected for " +
        'good, with no trial: if that front does not start, recovery may not be reachable. A ' +
        'trial is safer: write --new-front IMAGE starts it once first.',
    none: 'The file carries no waffle-boot front. Forced, it is written and selected: from then on ' +
        'the unit has no recovery (Power + M4 does nothing), and nothing on the unit brings ' +
        'waffle-boot back.',
};
export const SELECT_RISK = {
    19: 'This recovery began a trial write. Forced, the select comes before that trial start has ' +
        'run, so nothing shows that the new front comes up.',
    21: "The tried slot's front has left no mark of a trial start: nothing shows that it comes up. " +
        'Forced, it is selected all the same: if it does not start, recovery may not be reachable.',
};
const ACT_LABELS = { 1: 'Try:', 2: 'Switch:', 3: 'Keep:', 4: 'Put back:' };
const DONE_WORDS = {
    [WAY_TRY]: (s) => `${s} marked for one trial start`,
    [WAY_SWITCH]: (s) => `${s} is the boot slot`,
    [WAY_KEEP]: (s) => `${s} kept as the boot slot`,
    [WAY_PUT_BACK]: (s) => `${s}'s record made good again`,
};
export const TIER_WORDS = {
    [TIER_SAME]: 'holds an image with the front this unit runs',
    [TIER_FRONT]: 'holds another waffle-boot front: its recovery may not be this one, or may not open',
    [TIER_FOREIGN]: 'holds no waffle-boot front: there is no recovery there',
    [TIER_BAD]: 'failed its check: its record does not match its bytes, and nothing checks it at a start',
};

// --- small helpers ---

export const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
export const hex2 = (n) => (n & 0xFF).toString(16).padStart(2, '0');
export const hex8 = (n) => (n >>> 0).toString(16).padStart(8, '0');
export function sameBytes(a, b) {
    if (!a || !b || a.length !== b.length)
        return false;
    for (let k = 0; k < a.length; k++)
        if (a[k] !== b[k])
            return false;
    return true;
}
const BAD_SLOT = { good: false, has_desc: false, has_front: false, tried: false, started: false, te_prefix: false,
    length: 0, crc: 0, version: [0, 0, 0, 0], front_crc: 0, pin: new Uint8Array(32) };
// slot k of INFO; an index INFO cannot name reads as an empty bad slot
export const slotOf = (i, k) => i.slots[k] || BAD_SLOT;
const dmg = (i) => !!(i.spoiled || i.unsettled);

export function slotName(k) {
    return k === 0 || k === 1 ? `Slot ${k + 1}` : 'no slot';
}

// As the unit's screens show it: three places, and the build number after a '+' when not 0.
export function version(v) {
    return `${v[0]}.${v[1]}.${v[2]}` + (v[3] ? `+${v[3]}` : '');
}

// S-02.106: ['WB', version], ['TE', version], ['TE', null] or ['??', null] for a good or tried slot,
// from INFO's slot fields and flag bit 5; null for any other slot.
export function slotKind(s) {
    if (!s.good && !s.tried)
        return null;
    if (s.has_desc)
        return ['WB', s.version];
    const c = (s.tried ? ~s.crc : s.crc) >>> 0;
    const row = TE_BUILDS.find((r) => r[0] === s.length && r[1] === c);
    if (row)
        return ['TE', row[2]];
    return s.te_prefix ? ['TE', null] : ['??', null];
}

// A slot kind as the slot lines show it: 'WB v0.9.36', 'TE v1000.0.20' (with '+build' when the build is
// not 0), 'TE' or '??'.
export const kindText = (k) => (k[1] === null ? k[0] : `${k[0]} v${version(k[1])}`);

// S-15.151's state word: checked, tried, tried, started, or check failed.
export function slotState(s) {
    if (s.tried)
        return s.started ? 'tried, started' : 'tried';
    return s.good ? 'checked' : 'check failed';
}

export const v11 = (i) => i != null && (i.front || 0) >= ACTS_FRONT;
export const otherHint = () => OTHER_HINT;

// This recovery has made a write or an action, as INFO shows it.
export function afterWrite(i) {
    return i != null && (i.phase === DONE || !!i.moved ||
        (i.phase === FAILED && [9, 10, 11, 14].includes(i.result)));
}

// What a status means and what to do: statusText(st, { imageRc, hint, info }).
export function statusText(st, { imageRc = 0, hint = true, info = null } = {}) {
    const i = info;
    let [what, tip] = STATUS[st] || [`status 0x${hex2(st)}`, ''];
    if (st === 5)
        tip = otherHint(i);
    if ((st === 3 || st === 4) && afterWrite(i) && !dmg(i))
        [what, tip] = AFTER_WRITE;
    if (i != null && dmg(i) && RESTART_TIPS.includes(st))
        tip = '';
    if (st === 7 && IMAGE_RC[imageRc])
        what += ': ' + IMAGE_RC[imageRc];
    return what + (tip && hint ? '. ' + tip : '');
}

// A refusal of the start condition, before any image: INFO's refusals.
export function refusalText(st, i = null) {
    if (st === 12)
        return "the running slot's waffle-boot front is not intact in the flash (S-02.084)" +
            (i != null && dmg(i) ? '' : '. ' + otherHint(i));
    return statusText(st, { info: i });
}

export const selected = (index) => (index === 0 ? 0 : 1);     // S-02.011
export const waffle = (s) => s.has_desc && s.has_front;
export const imageWords = (s) => (waffle(s) ? `version ${version(s.version)}` : 'not a waffle-boot image');

// [state, words] of the PSoC's battery report (S-07.053). state: 'charged' (above 5 % and
// 3500 mV), 'low', 'absent' or 'unknown'.
export function battery(b) {
    if (!b || !b.length)
        return ['unknown', 'unknown: no report came from the unit'];
    if (b[0] === 0)
        return ['absent', 'absent: the cable alone powers the unit'];
    let words = b[0] === 2 ? 'charging' : 'not charging';
    if (b.length < 7)
        return ['unknown', `present, ${words}${b.length > 1 ? `, ${b[1]} %` : ''}, its voltage not reported`];
    const mv = (b[5] << 8) | b[6];
    words += `, ${b[1]} %, ${mv} mV`;
    if (b[1] > 5 && mv > 3500)
        return ['charged', `present and charged (${words})`];
    return ['low', `present but low (${words})`];
}

// S-02.084's damaged state: this recovery began a write of the running slot, and that slot is
// bad (flag bit 4), or read back good with the selector not settled on it while the other slot
// is bad (bit 7).
export const damaged = (i) => dmg(i) && i.phase !== WRITING;
// Flag bit 5: in the damaged state, a restart is not safe yet.
export const blocked = (i) => damaged(i) && i.held;
// A failure's hint is a plain write's, from a unit that starts as before.
export const plainHint = (i) => i.way !== WAY_SELECT_FIRST && i.way !== WAY_IN_PLACE && !damaged(i);

// The good slot a first stage reaches when its own selector write is not cut short, or null.
export function uncutReach(i) {
    if (!i.selector_ok || i.read_error)
        return null;
    const n = selected(i.sel_index);
    if (slotOf(i, n).good)
        return n;
    if (i.sel_verified !== 1 && slotOf(i, 1 - n).good)
        return 1 - n;
    return null;
}

export function stateLine(i) {
    const a = slotName(i.running), b = slotName(1 - i.running);
    if (i.spoiled)
        return `The failed write left ${a}, the running slot, damaged.`;
    return `A write of this recovery read ${a}, the running slot, back checked but did not settle the ` +
        `selector on it, and ${b} failed its check.`;
}

// Bit 5: why the unit holds, said as INFO lets it be said (S-02.084).
export function heldLines(i) {
    const n = uncutReach(i);
    let why;
    if (i.read_error) {
        why = 'What the flash holds is not known after a read that failed, so the unit claims no start.';
    } else if (n === null) {
        let reason;
        if (!slotOf(i, 0).good && !slotOf(i, 1).good)
            reason = 'no slot is checked';
        else if (i.selector_ok && i.sel_verified === 1)
            reason = `the selector trusts ${slotName(selected(i.sel_index))}, which is damaged`;
        else
            reason = 'no start falls back from the slot the selector names to a checked one';
        why = `The unit does not start at all if it is restarted or loses power now: ${reason}.`;
    } else {
        why = `A start that is not cut short would reach ${slotName(n)}, but a cut of the first ` +
            "stage's own selector write could leave a selector that leads to no checked slot, so a " +
            'restart is not safe yet.';
    }
    return [why, "The unit holds the power switch's power-off and refuses a restart. Keep the cable " +
        'in until a write has settled the selector (the ways out below); with no host, keep the ' +
        'unit powered.'];
}

// In the damaged state with bit 5 clear: which good slot a restart starts.
export function reachWords(i) {
    const n = selected(i.sel_index);
    if (slotOf(i, n).good)
        return `a restart starts ${slotName(n)}, which the selector names`;
    if (!slotOf(i, 1 - n).good)
        return 'the unit reports that a start reaches a checked slot';
    return `the first restart starts ${slotName(n)}, which the selector names and which is ` +
        `damaged: it may hang or open recovery again. The restart after it starts ${slotName(1 - n)}`;
}

// In the damaged state: the selector leads to the other slot, which is good, so the unit writes
// the running slot again for either request.
export const leadsToOther = (i) => damaged(i) && i.target === i.running;
// In the damaged state, the target already good: the only checked slot, which the unit takes only
// with the image it holds, to settle the selector on it (status 18).
export const onlySelect = (i, target) =>
    damaged(i) && (target === 0 || target === 1) && slotOf(i, target).good;
export const recordWords = (s) => `${imageWords(s)}, ${s.length} bytes, CRC-32 ${hex8(s.crc)}`;

// The writes that end the damaged state, each ending by settling the selector on the slot it wrote.
export function waysOut(i) {
    const a = i.running, b = 1 - i.running;
    const A = slotName(a), B = slotName(b);
    const sb = slotOf(i, b);
    if (leadsToOther(i)) {
        const words = `write and write --running both write ${A} again (way 1), with no selector ` +
            `write before it, and end by settling the selector on ${A}. ${B} is not touched`;
        if ((i.front || 0) >= SAME_FRONT)
            return words + '.';
        return words + ', and its own image is refused: there is nothing to do with it.';
    }
    let first, second;
    if (onlySelect(i, b)) {
        if (!(waffle(sb) && i.own_desc && sameBytes(sb.pin, i.pin)))
            first = `${B} holds ${waffle(sb) ? 'an image with another front' : 'no waffle-boot image'}` +
                ', which this recovery does not take as a file.';
        else
            first = `write with the image ${B} holds (${imageWords(sb)}) settles the selector on ` +
                `${B}: the slot is not erased, only the selector is written.`;
    } else {
        first = `write writes ${B}.`;
    }
    const way = i.run_way;
    if (way === WAY_PLAIN && onlySelect(i, a))
        second = `write --running with the image ${A} holds (${recordWords(slotOf(i, a))}) settles ` +
            `the selector on ${A}: the slot is not erased, only the selector is written; another ` +
            'image is refused.';
    else if (way === WAY_PLAIN)
        second = `write --running writes ${A} again (way 1), with no selector write before it.`;
    else if (way === WAY_SELECT_FIRST)
        second = `write --running settles the selector on ${B} first and rewrites ${A}.`;
    else if (way === WAY_IN_PLACE)
        second = `write --running writes ${A} again, in place.`;
    else
        second = `write --running is refused: ${refusalText(i.run_refusal, i)}`;
    return `${first} ${second} Each write ends by settling the selector on the slot it wrote.`;
}

// The state after an earlier write of the running slot in this recovery, and what to do.
export function retryLines(i) {
    const out = i.read_error ? [READ_ERROR] : [];
    if (!i.established || (i.running !== 0 && i.running !== 1))
        return out;
    const a = slotName(i.running), b = slotName(1 - i.running);
    if (damaged(i)) {
        out.push(stateLine(i));
        if (i.held)
            out.push(...heldLines(i));
        else
            out.push(`But ${reachWords(i)}. restart is accepted.`);
        out.push('Ways out: ' + waysOut(i));
        return out;
    }
    if (i.moved && i.selector_ok && selected(i.sel_index) !== i.running) {
        let line = `This recovery already selected ${b} before a write of ${a}, the running slot`;
        if (i.target === i.running)
            line += `: a write goes to ${a} again, with no selector write before it`;
        out.push(line + `. A restart now starts ${b}.`);
    }
    return out;
}

// What a write of the running slot would do.
export function runWords(i) {
    const a = slotName(i.running), b = slotName(1 - i.running);
    const way = i.run_way;
    if (way === WAY_PLAIN && onlySelect(i, i.running))
        return `would go to ${a}, which takes only the image it holds ` +
            `(${recordWords(slotOf(i, i.running))}), to settle the selector on it: the slot is not ` +
            'erased, only the selector is written';
    if (way === WAY_PLAIN && damaged(i))
        return `would go to ${a}, the running slot, again (way 1), with no selector write before it`;
    if (way === WAY_PLAIN)
        return `would go to ${a}, which the selector does not name now`;
    if (way === WAY_SELECT_FIRST)
        return `would go to ${a}, the running slot, after the selector names ${b}`;
    if (way === WAY_IN_PLACE)
        return `would go to ${a}, the running slot, in place, since ${b} failed its check: an interruption ` +
            'leaves a unit that does not start';
    return `refused: ${refusalText(i.run_refusal, i)}`;
}

// In the damaged state every write ends by settling the selector on its target.
export function settleLines(i, way, target) {
    if (!damaged(i))
        return [];
    const lines = [`The write ends by settling the selector on ${slotName(target)}.`];
    if (way === WAY_IN_PLACE && i.selector_ok && !i.read_error &&
            !(selected(i.sel_index) === target && i.sel_verified === 1))
        lines.push("The risk lasts until then, not only to the slot's last program: the selector " +
            `reads ${hex2(i.sel_index)} ${hex2(i.sel_verified)} now.`);
    return lines;
}

// --- the actions on the image a slot holds (front version 11) ---

export const otherOf = (i) =>
    (i.established && (i.running === 0 || i.running === 1) ? 1 - i.running : NO_SLOT);
// The slot an action acts on: the other slot, or for a keep the slot INFO names.
export const actSlot = (i, act) => (act === ACT_KEEP ? i.keep : otherOf(i));

function underWay(i) {
    if (ACT_NAMES[i.act])
        return `the ${ACT_NAMES[i.act]} of ${slotName(actSlot(i, i.act))}`;
    return 'the write';
}

// An action's refusal (S-02.084), said for that action, with what to do.
export function actText(act, st, i, hint = true) {
    const B = slotName(otherOf(i));
    const restartFirst = 'Restart first (wbrecover.py restart), with no key held.';
    const words = {
        [`${ACT_TRY},19`]: ['a try needs the running slot checked with its front intact, and none ' +
            'comes once this recovery has written the running slot or the selector', restartFirst],
        [`${ACT_SWITCH},7`]: [`${B}'s stream does not pass S-02.091's rules: the boot ROM would not ` +
            'start it', 'Write a bootable image to it first (wbrecover.py write IMAGE).'],
        [`${ACT_SWITCH},12`]: [`${B} does not hold the front this unit runs: a switch to it would make ` +
            'it the boot slot with no trial', 'Try it once first (wbrecover.py try).'],
        [`${ACT_SWITCH},19`]: ['this recovery has written the running slot or the selector', restartFirst],
        [`${ACT_SWITCH},21`]: [`${B} is tried, but its front has left no mark of a trial start: ` +
            'nothing shows that it comes up', 'Restart with no key held so that it starts once; ' +
            'then switch off and on, hold Power + M4 and switch or keep.'],
        [`${ACT_KEEP},19`]: ['this recovery has written the running slot or the selector, or begun ' +
            'a try or a trial write', restartFirst],
        [`${ACT_KEEP},20`]: ['no slot is tried where a keep can take it: from this front the other ' +
            'slot, tried, with the selector on the running slot with verified 01; in a trial start ' +
            'the running slot', 'Run info; a try comes first (wbrecover.py try).'],
        [`${ACT_KEEP},21`]: [`${B} is tried, but its front has left no mark of a trial start: ` +
            'nothing shows that it comes up', 'Restart with no key held so that it starts once; ' +
            'then switch off and on, hold Power + M4 and keep.'],
        [`${ACT_PUT_BACK},20`]: [`${B} is not tried: there is no trial mark to take off`, 'Nothing to do.'],
    };
    for (const act2 of [ACT_TRY, ACT_SWITCH]) {
        words[`${act2},22`] = [`${B} is already the boot slot (verified 01)`,
            'Nothing to do: restart the unit (wbrecover.py restart) to start it.'];
        words[`${act2},23`] = [`${B} failed its check and is not tried: nothing checks it at a start`,
            'Write an image to it first (wbrecover.py write IMAGE).'];
    }
    const got = words[`${act},${st}`];
    if (!got)
        return statusText(st, { hint, info: i });
    let [what, tip] = got;
    if (dmg(i) && tip.includes('estart'))
        tip = '';
    return what + (tip && hint ? '. ' + tip : '');
}

export function actGoal(i, act) {
    const k = slotName(actSlot(i, act));
    return {
        [ACT_TRY]: `would start ${k} once, at the next start`,
        [ACT_SWITCH]: `would make ${k} the boot slot`,
        [ACT_KEEP]: `would keep ${k}, with no image sent`,
        [ACT_PUT_BACK]: `would make ${k}'s record good again, with no selector write`,
    }[act];
}

// A keep would start now, of the slot a switch names: the slot came up from its trial, and the
// recovery menu offers its keep in place of the switch.
function keepAfterTrial(i) {
    return i.acts[ACT_KEEP - 1] === 0 && actSlot(i, ACT_KEEP) === actSlot(i, ACT_SWITCH);
}

// Front 11: the other slot's tier, and what each action would do now, unforced and forced.
export function actionLines(i) {
    if (!i.v11 || otherOf(i) === NO_SLOT)
        return [];
    const out = [];
    if (TIER_WORDS[i.tier])
        out.push(`Other:    ${slotName(otherOf(i))} ${TIER_WORDS[i.tier]}`);
    for (const act of [ACT_TRY, ACT_SWITCH, ACT_KEEP, ACT_PUT_BACK]) {
        const un = i.acts[act - 1], fo = i.forced[act - 1];
        let words;
        if (un === 0)
            words = actGoal(i, act);
        else if (fo === 0)
            words = `with --force only (${actText(act, un, i, false)}): ${actGoal(i, act)}`;
        else
            words = `refused: ${actText(act, fo, i, false)}`;
        if (act === ACT_SWITCH && keepAfterTrial(i))
            words += '; after its trial, keep is the action';
        out.push(ACT_LABELS[act].padEnd(10) + words);
    }
    return out;
}

function trialHint(i) {
    if (i.acts[ACT_TRY - 1] === 0)
        return ' A try starts it once first: wbrecover.py try.';
    if (i.forced[ACT_TRY - 1] === 0)
        return ' A try starts it once first: wbrecover.py try --force.';
    return '';
}

function slotWords(i, k) {
    const s = slotOf(i, k);
    return s.has_desc ? `version ${version(s.version)}` : 'no waffle-boot image';
}

// The warning before a switch for good, by the tier of the slot switched to.
export function switchLines(i, b) {
    const B = slotName(b), A = slotName(1 - b);
    const tier = i.tier;
    if (tier === TIER_BAD)
        return [`DANGER: ${B} failed its check: its record does not match its bytes. Switched to, the selector trusts ` +
            'it, and nothing will ever check this slot again: if its image does not start, there ' +
            `may be no way back without a flash programmer.${trialHint(i)}`];
    if (tier === TIER_FOREIGN)
        return [`WARNING: ${B} holds no waffle-boot front. Once switched, there is no recovery from ` +
            'there: Power + M4 does nothing, and nothing on the unit brings waffle-boot back.' +
            trialHint(i)];
    if (tier === TIER_FRONT)
        return [`WARNING: ${B} holds another waffle-boot front (${slotWords(i, b)}). Once switched, ` +
            'recovery may not be reachable from it: if that front does not start, or its recovery ' +
            `is not this one, there may be no way back to ${A}.${trialHint(i)}`];
    return [`${B} holds an image with the front this unit runs (${slotWords(i, b)}). A switch makes ` +
        `it the boot slot: every start boots ${B} from now on.${trialHint(i)}`];
}

// What a try of slot b does, by its tier.
export function tryLines(i, b) {
    const B = slotName(b), A = slotName(1 - b);
    const lines = [`The next start runs ${B} once, as it lies (${slotWords(i, b)}); the starts ` +
        `after it boot ${A} again, until a keep (wbrecover.py keep) after that start.`];
    if (i.tier === TIER_FRONT)
        lines.push(`${B} holds another waffle-boot front: its recovery may not open in that start. ` +
            `If it does not come up, switch the unit off and on: ${A} boots again.`);
    else if (i.tier === TIER_FOREIGN)
        lines.push(`${B} holds no waffle-boot front: there is no recovery in that start. Switch the ` +
            `unit off and on to come back to ${A}.`);
    else
        lines.push(`If it does not come up, switch the unit off and on: ${A} boots again.`);
    if (b === 1)
        lines.push('Slot 2 has two short windows: a power cut during the selector erase of this ' +
            'try, or in the first instant of the trial start, can leave Slot 2 trusted before its ' +
            'image has run; if it then does not start, the unit is lost.');
    return lines;
}

export function keepLines(i, k) {
    const K = slotName(k), other = slotName(1 - k);
    if (k === i.running) {
        const lines = [`This is the trial start of ${K}. The keep makes ${K} the boot slot: its ` +
            `record is made good and the selector names it; its image is not written again, and ` +
            `${other} is kept as it is.`];
        if (i.why === 2)
            lines.push('WARNING: recovery runs because the image check failed: this image runs, but ' +
                'does not pass its own check. Keeping it leaves a unit that starts only into ' +
                'recovery.');
        return lines;
    }
    const lines = [`${K} is tried; the keep makes it the boot slot: its record is made good and ` +
        'the selector names it, to be checked at the next start. Its image is not written again, ' +
        `and ${other} is kept as it is.`];
    if (i.tier === TIER_FRONT || i.tier === TIER_FOREIGN)
        lines.push(`${K} ${TIER_WORDS[i.tier]}.`);
    return lines;
}

export function putBackLines(i, b) {
    const B = slotName(b), A = slotName(1 - b);
    const sel = selKnown(i);
    let line = `${B}'s trial mark is taken off: its record is made good again, as before the ` +
        'trial, and its image is not written.';
    if (sel !== null && sel[0] === b && sel[1] !== 1 && !damaged(i))
        line += ` The selector names ${B} for a try that has not started; it is first settled ` +
            `back on ${A}.`;
    else
        line += ' The selector is not written.';
    return [line];
}

// The warning before a forced action: the refusal lifted, and its risk.
export function forceLines(act, st, i) {
    const A = slotName(i.running), B = slotName(otherOf(i));
    const lines = ['Without --force the unit refuses this: ' +
        `${actText(act, st, i, false).replace(/\.+$/, '')}.`];
    if (st === 3 || st === 4)
        lines.push(`Forced, the ${ACT_NAMES[act]} goes on over the selector as it reads now ` +
            `(${hex2(i.sel_index)} ${hex2(i.sel_verified)}), which a start has not settled yet.`);
    else if (act === ACT_TRY && st === 19)
        lines.push(`Forced, the trial start comes back to ${A} all the same: if ${B} does not come ` +
            `up, switching off and on starts ${A}, which may not start either.`);
    else if (st === 23)
        lines.push(`Forced, the unit acts on ${B} as it lies: nothing has checked its image.`);
    else if (st === 21)
        lines.push(`Forced, ${B} is kept as the boot slot although its front never showed that it starts: ` +
            'if it does not, recovery may not be reachable.');
    else if (st === 19 || st === 20)
        lines.push(`Forced, the ${ACT_NAMES[act]} goes on from the state this recovery left.`);
    return lines;
}

// Front 11: what follows the confirmation, before the first erase.
export function countdownNote(i) {
    if (!v11(i))
        return [];
    if (i.held)
        return ['The unit begins at once, with no countdown: it holds the power-off while a write of ' +
            'this recovery has left no checked slot to start.'];
    return [`Once confirmed, the unit counts down ${COUNTDOWN} s before its first erase: switching ` +
        'it off or unplugging USB in that time stops it, with the flash untouched.'];
}

// [the slot the selector names, its verified byte], or null when it was not read.
export function selKnown(i) {
    if (!i.selector_ok || i.read_error)
        return null;
    return [selected(i.sel_index), i.sel_verified];
}
const selIs = (i, slot, verified) => {
    const s = selKnown(i);
    return s !== null && s[0] === slot && s[1] === verified;
};

// This recovery runs in a trial start (S-02.022): the running slot is tried and the selector
// names the other slot with verified 01.
export function trialStart(i) {
    const a = i.running;
    return !!(i.established && (a === 0 || a === 1) && slotOf(i, a).tried && selIs(i, 1 - a, 1));
}

const keeps = (i) => (v11(i) ? 'wbrecover.py keep (or select IMAGE)' : 'select IMAGE');

// This recovery began a trial write: a select is then refused with status 19 until a restart.
export function trialBegun(i) {
    return i.way === WAY_TRIAL && (i.phase === WRITING || i.phase === DONE ||
        (i.phase === FAILED && [9, 10, 11].includes(i.result)));
}

// Why the old front would not take a select now, or ''.
export function selectBlocked(i) {
    const a = i.running;
    if (trialBegun(i))
        return 'this recovery began a trial write, so a select needs a restart first (status 19)';
    if (damaged(i) || i.moved)
        return `this recovery wrote the selector or began a write of ${slotName(a)} (status 19)`;
    if (!selIs(i, a, 1))
        return `the selector does not read ${slotName(a)} with verified 01`;
    if (!slotOf(i, a).has_front)
        return `${slotName(a)}'s front is not intact`;
    return '';
}

// What a tried slot means in the state INFO reports.
export function triedLines(i, failedTrial = false) {
    const a = i.running;
    if (!i.established || (a !== 0 && a !== 1))
        return [];
    const sel = selKnown(i);
    const A = slotName(a), B = slotName(1 - a);
    if (slotOf(i, a).tried) {
        if (trialStart(i))
            return [`This is the trial start of ${A}: ${keeps(i)} keeps it; switching off and on ` +
                `starts ${B} again.`];
        if (sel === null)
            return [`${A} is tried and running, but the selector could not be read, so the tool ` +
                'cannot tell whether this is a trial start: run info again before a select.'];
        if (sel[0] === a && sel[1] === 1 && v11(i))
            return [`${A} is tried, but the selector trusts it although it never passed a trial: ` +
                'every start boots it, and a select is refused. To leave this state, switch to ' +
                `${B} from here (wbrecover.py switch, ${SLOT_MENU}), if it is checked and holds a ` +
                `stream the boot ROM can start; from its recovery, wbrecover.py try starts ${A} ` +
                'once, or a plain write replaces it.'];
        if (sel[0] === a && sel[1] === 1)
            return [`${A} is tried, but the selector trusts it although it never passed a trial: ` +
                'every start boots it, and a select is refused. From its recovery, write ' +
                `--new-front IMAGE again for a trial, or a plain write replaces ${A}.`];
        if (sel[0] === a)
            return [`${A} is tried and running, and the selector still names it unverified ` +
                `(verified ${hex2(sel[1])}): this trial start did not finish its selector step. ` +
                `The next start is another trial start of ${A}, and the select works there.`];
        return [`${A} is tried and running, but the selector names ${B} with verified ` +
            `${hex2(sel[1])}, not 01, so this is not a trial start: a select is refused here.`];
    }
    const t = 1 - a;
    if (!slotOf(i, t).tried)
        return [];
    const again = v11(i)
        ? 'Try it again with wbrecover.py try (the image it holds) or write --new-front IMAGE, ' +
            'then restart with no key held.'
        : 'Write it again with --new-front, then restart with no key held.';
    if (sel !== null && sel[0] === t && sel[1] !== 1) {
        if (failedTrial)
            return [`The failed trial write left ${B} tried, with the selector naming it ` +
                'unverified: the next start runs it once, a trial start, and a select comes after ' +
                'that start (in this recovery it is refused).'];
        return [`${B} is tried: the next start runs it once, its trial start.`];
    }
    if (slotOf(i, t).started) {
        if (failedTrial)
            return [`The failed trial write did not reach ${B}'s record: the earlier trial's mark ` +
                `still stands for the image ${B} held before. A select of that image needs a ` +
                'restart first (in this recovery it is refused, status 19).'];
        const why = selectBlocked(i);
        if (why)
            return [`${B} is tried and its front marked its trial start, but a select is not taken ` +
                `now: ${why}.`];
        return [`${B} is tried and its front marked its trial start: ${keeps(i)} keeps it.`];
    }
    const out = [];
    if (failedTrial)
        out.push(`The failed trial write left ${B} carrying the trial mark: it cannot be selected.`);
    if (sel === null)
        return out.concat([`${B} is tried; the selector could not be read.`]);
    const [n, v] = sel;
    if (n === t && v11(i))
        return out.concat([`${B} is tried, but the selector trusts it although it never passed a ` +
            `trial: the next start, and every one after, boots it. To end this, switch to ${A} ` +
            `from ${B}'s recovery (wbrecover.py switch, ${SLOT_MENU}); from here, wbrecover.py ` +
            'put-back makes its record good again, as it was before the trial.']);
    if (n === t)
        return out.concat([`${B} is tried, but the selector trusts it although it never passed a ` +
            'trial: the next start, and every one after, boots it.']);
    if (v === 1) {
        let line = `${B} is tried, its front has left no mark of a trial start, and the selector ` +
            `trusts ${A}, so no trial start will come: a select is refused. ${again}`;
        if (!failedTrial)
            line += ' If the unit did not come up in that trial start, do not write the same image ' +
                `again: its front does not start; rebuild it. A newer front in ${A} also marks ` +
                'such a trial as spent on its next start, which the tool sees only as not started: ' +
                'either way the trial write must be repeated to try again.';
        return out.concat([line]);
    }
    return out.concat([`${B} is tried, but the selector names ${A} (verified ${hex2(v)}): the next ` +
        `start checks ${A} and boots it, so no trial start will come. ${again}`]);
}

// INFO's device (front 20, S-18.004), as the Python tool's device_line() says it.
export function deviceLine(i) {
    const name = DEVICES[i.device] || `device ${i.device}`;
    if (i.dev_how === 0)
        return `Device:   ${name}, identified by its first stage`;
    return `Device:   not identified (first stage n ${i.fs_n}, c ${(i.fs_c >>> 0).toString(16).padStart(8, '0')}): ` +
        `recovery runs as on the ${name}`;
}

// INFO as lines a person reads: the Python tool's info, line for line.
export function describeLines(i) {
    const out = [`Recovery runs because ${WHY[i.why] || `of reason ${i.why}`}.`];
    if (i.v20)
        out.push(deviceLine(i));
    const sel = i.selector_ok ? selected(i.sel_index) : null;
    if (i.established)
        out.push(`Running:  ${slotName(i.running)}`);
    else
        out.push('Running:  not established (the loaded image matches neither slot whole, or ' +
            'both alike)');
    if (i.selector_ok)
        out.push(`Selector: names ${slotName(sel)}, verified ${hex2(i.sel_verified)}` +
            (i.sel_verified === 1 ? '' : ' (not settled: the next start checks it)') +
            (i.moved ? ' (this recovery selected it, before a write of the running slot)' : ''));
    else
        out.push('Selector: could not be read');
    if (i.own_desc)
        out.push(`Front:    the running image's version ${version(i.version)}, front pin ${hex(i.pin)}`);
    else
        out.push("Front:    the running image's descriptor was not found");
    i.slots.forEach((s, k) => {
        const tags = [['running', i.established && i.running === k], ['boot slot', sel === k]]
            .filter((t) => t[1]).map((t) => t[0]);
        const kind = slotKind(s);
        let line = `${slotName(k)}:   ${slotState(s)}`;
        if (kind !== null)
            line += `, ${kindText(kind)}, ${s.length} bytes, CRC-32 ${hex8(s.crc)}`;
        else if (s.has_desc)
            line += `, version ${version(s.version)}`;
        if (s.has_desc) {
            line += `, front ${s.has_front ? 'present' : 'damaged'}`;
            line += ` (pin ${hex(s.pin.subarray(0, 8))}...)`;
        }
        if (tags.length)
            line += ` [${tags.join(', ')}]`;
        out.push(line);
    });
    if (i.target === 0 || i.target === 1) {
        let line = `Write:    would go to ${slotName(i.target)}`;
        const t = slotOf(i, i.target);
        if (onlySelect(i, i.target))
            line += `, which takes only the image it holds (${imageWords(t)}, ${t.length} bytes, ` +
                `CRC-32 ${hex8(t.crc)}) and is then selected: the slot is not erased, only the ` +
                'selector is written';
        else if (leadsToOther(i))
            line += ', the running slot, again (way 1), with no selector write before it';
        else if (t.good)
            line += `, replacing a checked image (${imageWords(t)})`;
        out.push(line);
    } else {
        out.push(`Write:    refused: ${refusalText(i.refusal, i)}`);
    }
    if (i.v6)
        out.push(`          with --running: ${runWords(i)}`);
    out.push(...actionLines(i));
    if (i.v6)
        out.push(`Battery:  ${battery(i.batt)[1]}`);
    out.push('State:    ' + phaseWords(i));
    if (i.refused)
        out.push(`Since:    ${i.refused === 24 ? 'the next write or action'
            : 'the last request after it was refused'}: ${statusText(i.refused, { info: i })}`);
    const failed = i.phase === FAILED && i.way === WAY_TRIAL && [9, 10, 11].includes(i.result);
    return out.concat(triedLines(i, failed), retryLines(i));
}

// The write's or the action's state, as info's "State:" line says it.
export function phaseWords(i) {
    const ph = i.phase;
    let state = ph < PHASES.length ? PHASES[ph] : `phase ${ph}`;
    if (ph === RECEIVING) {
        state += `, ${i.got} of ${i.len} bytes`;
    } else if (ph === COUNTING) {
        state += `: ${i.count} s left before the first erase of ${underWay(i)}. Switching the unit ` +
            'off or unplugging USB now stops it, with nothing written';
    } else if (ph === WRITING) {
        state += ({ [WAY_IN_PLACE]: ' in place', [WAY_TRIAL]: ' for one trial start',
            [WAY_SELECT]: ' a select', [WAY_TRY]: ': a try', [WAY_SWITCH]: ': a switch',
            [WAY_KEEP]: ': a keep', [WAY_PUT_BACK]: ': a put-back' }[i.way] || '') +
            `, ${(i.progress / 10).toFixed(1)}%`;
    } else if (ph === DONE && DONE_WORDS[i.way]) {
        state += `: ${DONE_WORDS[i.way](slotName(i.written))}`;
    } else if (ph === DONE) {
        state += `: ${slotName(i.written)} ` +
            ({ [WAY_TRIAL]: 'written for one trial start', [WAY_SELECT]: 'selected' }[i.way] || 'written') +
            ` and read back, SHA-256 ${hex(i.readback)}`;
    } else if (ph === FAILED) {
        state += `: ${statusText(i.result, { imageRc: i.image_rc, hint: plainHint(i), info: i })}`;
    }
    return state;
}

// The earlier write a refusal leaves in INFO.
export function madeWords(i) {
    if (i.phase === DONE && DONE_WORDS[i.way])
        return DONE_WORDS[i.way](slotName(i.written));
    if (i.phase === DONE)
        return `${slotName(i.written)} written and read back`;
    return `${i.act ? 'an action' : 'a write'} that failed: ${statusText(i.result, { hint: false })}`;
}

// --- the warnings before a write ---

// The warning for the write BEGIN announced (this way, to this target), from INFO.
export function confirmLines(i, way, target) {
    if (way === WAY_IN_PLACE)
        return inPlaceLines(i, target);
    return baseLines(i, way, target).concat(settleLines(i, way, target));
}

function baseLines(i, way, target) {
    const a = i.running, b = 1 - i.running;
    const A = slotName(a), B = slotName(b);
    const sa = slotOf(i, a), sb = slotOf(i, b);
    const held = i.held ? heldLines(i) : [];
    if (way === WAY_SELECT_FIRST) {
        const lines = [`This erases and rewrites ${A}, the running slot. The unit first selects ` +
            `${B}: if the write is interrupted, the unit starts ${B}'s image (${imageWords(sb)}).`];
        if (!waffle(sb))
            lines.push('The unit is then on that bootloader, without this recovery.');
        return lines;
    }
    if (onlySelect(i, target)) {
        const T = slotName(target);
        return [`This settles the selector on ${T}, which already holds this image ` +
            `(${imageWords(slotOf(i, target))}). The slot is not erased: the unit reads it back and ` +
            `then writes only the selector, to name ${T}. ${T} keeps its image if this is ` +
            'interrupted.'].concat(held);
    }
    if (target === a && damaged(i))
        return [`The unit announced a write of ${A}, the running slot, with no selector write ` +
            `before it (way 1): the selector leads to ${B}, which is checked and is not touched.`].concat(held);
    if (target === a) {
        const lines = [`This write goes to ${A}, the running slot, with no selector write before ` +
            `it: ${B} is selected, and an interruption starts ${B}'s image (${imageWords(sb)}).`];
        if (sa.good)
            lines.push(`${A} holds a checked image (${imageWords(sa)}), which this write replaces.`);
        return lines;
    }
    if (damaged(i))
        return [stateLine(i) + ' An interruption of this write leaves no settled selector either: ' +
            'until a write has ended well, a restart is not safe.'].concat(held);
    if (sb.good) {
        const lines = [`${B} holds a checked image (${imageWords(sb)}), which this write replaces.`];
        if (i.v6 && !sa.good)
            lines.push(`${A}, the running slot, failed its check, so ${B} holds the only checked image. write ` +
                `--running is the safer choice in this state: it selects ${B} first and rewrites ` +
                `${A}, so an interruption starts ${B}.`);
        return lines;
    }
    return [`${B} holds no checked image, so nothing checked is lost. ${A}, the running slot, is left as ` +
        'it is: if the write is interrupted, the unit starts as before.'];
}

// The warning before an in-place write, with the battery report INFO carries.
export function inPlaceLines(i, target = null) {
    const a = slotName(i.running), b = slotName(1 - i.running);
    const [state, words] = battery(i.batt);
    const lines = [`DANGER: ${b} holds no checked image, so this write erases ${a}, the running slot, ` +
        'and writes it in place.',
        'If the write is interrupted (a power cut, a pulled cable with no charged battery, the ' +
        'power switch), the unit cannot be repaired without a flash programmer.'];
    if (damaged(i)) {
        lines.push(`${a} is already damaged by a failed write of this recovery.`);
        if (i.held)
            lines.push(...heldLines(i));
        lines.push(...settleLines(i, WAY_IN_PLACE, target === null ? i.running : target));
    } else {
        lines.push(`The unit offers this only because a write of ${b} failed in this recovery.`);
    }
    lines.push(`Battery: ${words}.`);
    if (state === 'charged')
        lines.push('The battery carries the unit if the cable comes out. Do not touch the power ' +
            'switch until the write has ended.');
    else
        lines.push('Keep the cable in and do not touch the power switch until the write has ended.');
    return lines;
}

export const shortPin = (pin) => hex(pin.subarray(0, 8)) + '...';

// The warning before a trial write (way 4) to target. front: the file's descriptor;
// signed: [state ('signed', 'unsigned' or 'unknown'), the front's version or null, words].
export function trialLines(i, front, target, signed) {
    const [state, ver, words] = signed;
    const lines = ["The file's front is not the one this unit runs.",
        `  running: front ${i.front || 0}, pin ${shortPin(i.pin)}   ` +
            `file: pin ${shortPin(front.pin)}, ${words}`];
    if (state === 'unsigned')
        lines.push('WARNING: that front is not among the front pins this page knows as signed, so ' +
            'nothing here says it was built from this tree or has ever started on a unit. Go on ' +
            'only if you know where the file comes from.');
    else if (state === 'unknown')
        lines.push('Make sure of where the file comes from before going on.');
    if (ver === null)
        lines.push(oldFrontRisk(slotName(target)));
    const t = slotOf(i, target);
    if (t.good || t.tried)
        lines.push(`${slotName(target)} holds an image (${imageWords(t)}), which this write ` +
            'erases: it is lost, whether or not the new front starts.');
    else
        lines.push(`${slotName(target)} holds no checked image, so nothing checked is lost.`);
    lines.push(`It will be written to ${slotName(target)} and started ONCE. If the unit does not ` +
        `come up, switch it off and on: it boots ${slotName(i.running)} again. Nothing is selected ` +
        'until a select (wbrecover.py select IMAGE).');
    if (target === 1)
        lines.push('Slot 2 has two short windows: a power cut during the selector erase at the end ' +
            'of this write, or in the first instant of the trial start, can leave Slot 2 trusted ' +
            'before its front has run; if that front then does not start, the unit is lost.');
    return lines;
}

// What a select of slot k does, before its yes.
export function selectLines(i, k) {
    const m = slotName(k), other = slotName(1 - k);
    if (trialStart(i)) {
        const lines = [`This is the trial start of ${m}, the new front, and it came up. The select ` +
            `makes ${m} the boot slot: its record is made good and the selector names it; its ` +
            `image is not written again, and ${other} is kept as it is.`];
        if (i.why === 2)
            lines.push('WARNING: recovery runs because the image check failed: the new front runs, ' +
                'but its image does not pass its own check. Selecting it leaves a unit that starts ' +
                'only into recovery.');
        return lines;
    }
    return [`${m} holds the new front, tried, and the unit recorded the new front's start in its ` +
        `trial. The select makes ${m} the boot slot: its record is made good and the selector ` +
        `names it; its image is not written again, and ${other} is kept as it is.`];
}

// What the action does to slot k, before its confirmation.
export function actConfirmLines(i, act, k) {
    if (act === ACT_TRY)
        return tryLines(i, k);
    if (act === ACT_SWITCH)
        return switchLines(i, k);
    if (act === ACT_KEEP)
        return keepLines(i, k);
    return putBackLines(i, k);
}
