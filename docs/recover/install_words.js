// SPDX-License-Identifier: GPL-2.0-or-later
//
// The installer's numbers and words: its request types, the fields of its replies, and every
// sentence the page says about them. install.js imports this file and exports its names again.

// The ferret tool's page, for a unit that has never had developer mode on.
export const FERRET_URL = 'https://bagofwaffles.github.io/ferret/op-xy/';

export const INSTALLER_FILE = 'waffle-install.ldr';

export const IHELLO = 0x40, STATE = 0x41, TEST = 0x42, REPORT = 0x43;
export const MAGIC = [0x57, 0x42, 0x49, 0x4E];          // "WBIN"
export const IPROTO = 1;
export const IHELLO_LEN = 64, STATE_HEAD = 32, REPORT_CHUNK = 1024;
export const UNKNOWN = 0xFF;                            // the reply to a type it does not know
export const NONE = 0xFF;

// flags
export const F_CHECKS_DONE = 1, F_UNLOCKED = 2, F_IMAGE_OK = 4, F_BREADCRUMB = 8;

// slot state bits
export const SB_GOOD = 0x01, SB_TRIAL_IMAGE = 0x02, SB_COMMITTED = 0x04, SB_PENDING = 0x08,
    SB_STARTED = 0x10, SB_SPENT = 0x20, SB_ODD = 0x40, SB_REACHED = 0x80;

export const STARTING = 0, CHECKING = 1, GUIDED = 2, LOCKED = 3, READY = 4, WRITING_TRIAL = 5,
    TRIAL_WRITTEN = 6, COMMIT_READY = 7, COMMITTING = 8, DONE = 9, FAILED = 10, TRIAL_FAILED = 11,
    LEFT = 12;

// [name, what the page says]
export const PHASE = {
    0: ['starting', 'Starting the checks'],
    1: ['checking', 'Running the checks'],
    2: ['guided', 'The key check: press com, then M4 (4 under the display), on the unit'],
    3: ['locked', 'A check did not pass: nothing was written'],
    4: ['ready', 'Hold 1 on the unit for 5 s to write the trial'],
    5: ['writing trial', 'Writing the trial'],
    6: ['trial written', 'Switch off, then on: the new bootloader starts once'],
    7: ['commit ready', 'Hold 1 for 5 s to keep the new bootloader'],
    8: ['committing', 'Writing'],
    9: ['done', 'Done: switch off and on'],
    10: ['failed', 'A write failed'],
    11: ['trial failed', 'The trial failed'],
    12: ['left', 'Nothing written'],
};

export const VERDICT = { 0: 'PASS', 1: 'PASS WITH GAPS', 2: 'FAIL', 0xFF: 'not yet' };
export const TEST_STATE = ['not run', 'pass', 'FAIL', 'not tested', 'not passed'];
export const TEST_KIND = ['OS', 'automatic', 'guided'];

export const LOCK = {
    1: 'a check failed',
    2: 'a check was not tested',
    3: 'the installer did not verify the image it carries (CRC-32 or front pin)',
    4: 'the slot the selector names is not verified or not good',
    5: "the unit's OS was not found, or fails its check",
    6: 'the key check (com and M4) was ended with 8',
    7: 'the eMMC could not be reached',
};

// what to do, by lock reason
export const LOCK_ADVICE = {
    1: 'Do not install on this unit until every check passes: check.txt says which one failed.',
    2: 'Start the installer again so that every check runs: check.txt says which one was not tested.',
    3: `The installer file may be damaged: get ${INSTALLER_FILE} again and write it once more.`,
    4: 'The bootloader slots are not in a state the installer writes over.',
    5: "The installer needs the unit's OS in the other boot partition, checked, so that it can " +
        'make it active again. Write the OS from the developer menu, then the installer again.',
    6: 'Start the installer again and press com, then M4 (4 under the display), on the unit.',
    7: 'Switch off and on and start the installer again. Save check.txt if it happens again.',
};

export const OS_PART = {
    0: 'not looked at yet',
    1: 'found and checked',
    2: 'found, stored encrypted (not checked)',
    3: 'none found',
    4: 'found, and its check fails',
    5: 'could not be read',
    6: 'made active again',
    7: 'making it active again failed',
};

export const TRIAL = {
    0: 'none',
    1: 'written, waiting for the trial start',
    2: 'the trial start reached the boot menu',
    3: 'the trial start ran but did not reach the boot menu',
    4: 'the trial start did not run the new bootloader',
};

export const FAILURE = {
    2: 'a read, erase or program of the flash failed',
    3: 'the image is the wrong size',
    4: 'a read-back did not match what was written',
    5: 'the selector is not verified, or the selected slot is not good',
    6: 'the slot is not good',
    7: 'the image is not a bootable stream',
    0x40: 'commit refused: this installer is a breadcrumb build',
    0x41: 'commit refused: the trial entry is not started',
    0x43: 'the installer did not verify the image it carries (CRC-32 or front pin)',
    0x44: 'commit refused: the trial start did not reach the boot menu',
};

export const SLOT_BITS = [
    [SB_GOOD, 'good'], [SB_TRIAL_IMAGE, 'holds this image for a trial'],
    [SB_COMMITTED, 'holds this image, kept'], [SB_PENDING, 'trial pending'],
    [SB_STARTED, 'trial started'], [SB_SPENT, 'trial spent'], [SB_ODD, 'trial entry odd'],
    [SB_REACHED, "trial reached the boot menu"],
];

export const STEADY = 'Keep the unit on and the cable in.';
export const LEAVE_KEY = 8;
// the phases in which the installer offers to leave, when the OS can be made active again
export const LEAVE_PHASES = [3, 4, 7, 9, 10, 11];
export const LEAVE = 'To leave without installing, hold 8 on the unit for 5 s: the installer makes ' +
    'the OS active again.';
export const BUSY = 'The unit is busy and does not answer for now; this page keeps waiting. Keep the ' +
    'unit on and the cable in.';
export const GONE_TRIAL = 'The port went away: that is expected once the unit is off for the trial ' +
    'start. Switch it on: when the installer is back, this page connects again.';
export const GONE_END = 'The port went away at switch-off, as expected: the unit starts its OS now, ' +
    'not the installer.';
export const SAVE_REPORT = 'Save check.txt below and keep it: it lists every check.';
export const OS_BACK = 'The OS is active again: switch the unit off and on and it starts its OS.';
export const OS_BACK_FAILED = 'Making the OS active again failed: the unit starts the installer ' +
    'again at the next start. Write the OS again from the developer menu.';
export const OS_STAYS = 'The installer is still the active OS: the unit starts it again at the ' +
    'next start.';
export const BREADCRUMB = 'This installer is a breadcrumb build: it writes the trial, and refuses ' +
    'the commit.';

export const NAMES = ['Slot 1', 'Slot 2'];
export const slotName = (k) => (k === 0 || k === 1 ? NAMES[k] : 'no slot');
export const testState = (s) => TEST_STATE[s] || `state ${s}`;
export const lockText = (r) => LOCK[r] || `lock reason ${r}`;
export const osText = (o) => OS_PART[o] || `state ${o}`;
export const trialText = (t) => TRIAL[t] || `state ${t}`;
export const hex8 = (n) => (n >>> 0).toString(16).padStart(8, '0');
export const failureText = (c) => FAILURE[c] || `failure code 0x${c.toString(16).padStart(2, '0')}`;
export const slotBits = (b) => SLOT_BITS.filter(([m]) => b & m).map(([, w]) => w);

// --- sending the installer over the boot menu's developer link ---
// A unit already on waffle-boot, with developer mode on, takes the file on its USB serial port in
// the boot menu, as tools/testos_mon.py dev-put sends it (docs/devhost.md). The numbers are that
// tool's; the port is the same USB vendor and product as recovery's and the installer's.

export const DEV_ROUTE = 0x21;          // S-06.010: the host to the master
export const DEV_ID = 0x7D;             // S-10c.110: the developer requests
export const DEV_PUT = 25, DEV_SHA = 26;
export const PUT_OPEN = 0, PUT_DATA = 1, PUT_CLOSE = 2;
export const PUT_CHUNK = 240;           // file bytes in one PUT_DATA, as the tool sends them
export const DEV_NAME = 'os.ldr';       // the only name the boot menu takes
export const DEV_TIMEOUT = 5000, DEV_SHA_TIMEOUT = 30000;   // ms, the tool's
export const DEV_STATUS = { 0: 'ok', 1: 'refused', 2: 'bad request', 3: 'the slave is not answering' };
export const LDR_MAX = 0x800000;        // the most a unit loads (docs/test-os.md)
export const OS_WRITE_KEY = 5;

export const DEV_TITLE = 'Or send it from here, to a unit already on waffle-boot';
export const DEV_HINT = 'For a unit whose bootloader is waffle-boot, with developer mode on (Settings, 1 ' +
    'Developer mode): this page sends the file to the unit over its USB serial port while the boot ' +
    'menu shows, then checks the unit\'s copy. Nothing is written to a boot partition until you press ' +
    `${OS_WRITE_KEY} in the developer menu yourself.`;
export const DEV_DROP = 'Drop waffle-install.ldr here';
export const DEV_DROP_SUB = '.ldr, a boot stream of at most 8 MB';
export const DEV_SEND = 'Send to the boot menu';
export const DEV_PICK = 'The browser asks which port to use: pick the OP-XY (it may be named OP-XY Boot).';
export const DEV_OTHER = 'A unit on another bootloader has no such link: write the file from its own ' +
    'developer menu, as above.';

export const DEV_EMPTY = 'The file is empty.';
export const devTooLarge = (n) => `The file is ${n.toLocaleString('en-US')} bytes, more than a unit loads ` +
    `(${LDR_MAX.toLocaleString('en-US')} bytes).`;
export const DEV_NOT_LDR = 'The file is not a boot stream: its first block header is not one (signature ' +
    `0xAD, header checksum zero). Choose ${INSTALLER_FILE}.`;
export const devOtherName = (name) => `The file is named ${name}, not ${INSTALLER_FILE}: check that it ` +
    'is the installer before you press 5.';
export const devFileOk = (n) => `A boot stream of ${n.toLocaleString('en-US')} bytes.`;

export const DEV_STAGE = {
    open: `Opening /yaffs2/${DEV_NAME} on the unit...`,
    sending: 'Sending',
    close: 'Closing the file...',
    sha: "Checking the unit's copy (SHA-256)...",
};
export const devSending = (n, total) => `Sending: ${n.toLocaleString('en-US')} of ` +
    `${total.toLocaleString('en-US')} bytes`;

export const DEV_VERIFIED = 'The unit has the file: its size and SHA-256 match.';
export const DEV_VERIFIED_ADVICE = [
    `In the developer menu (3 on the boot menu), press ${OS_WRITE_KEY}, Write /os.ldr to boot ` +
        'partition. It writes the file into the boot partition the OS is not in.',
    'When the unit says the OS is written, switch it off and on (or press 6): the installer starts. ' +
        'Then connect to it below.',
];
export const devMismatch = (unitSha, unitSize, sha, n) => "The unit's copy does not match the file: " +
    `SHA-256 ${unitSha} and ${unitSize.toLocaleString('en-US')} bytes on the unit, ${sha} and ` +
    `${n.toLocaleString('en-US')} bytes here.`;
export const DEV_DONT_WRITE = `Do not press ${OS_WRITE_KEY} in the developer menu: the file on the unit ` +
    'is not this one.';
export const DEV_AGAIN = 'Send the file again.';
export const DEV_REFUSED = {
    1: 'The boot menu refused the request: developer mode is off, or the file was refused. Turn on ' +
        'Settings, 1 Developer mode, and send again.',
    2: 'The boot menu took the request as malformed.',
    3: 'The second processor is not answering, so the unit cannot reach its drive: switch the unit off ' +
        'and on, then send again.',
};
export const devRefused = (status) => DEV_REFUSED[status] || `The boot menu answered status ${status}.`;
export const DEV_NO_ANSWER = 'No answer from the boot menu. Check that the unit shows the boot menu, with ' +
    'developer mode on, and that the port picked is the OP-XY. Turning MTP on or off re-enumerates USB: ' +
    'send again to pick the port again.';
export const DEV_GONE = 'The port went away while the file was sent.';
export const DEV_PARTIAL = `The file on the unit is incomplete. Do not press ${OS_WRITE_KEY} in the ` +
    'developer menu; send again.';
export const DEV_NO_PORT = 'No port was chosen.';
export const devOpenFailed = (why) => `Could not open the port: ${why}. Close any other program using ` +
    'it, then send again.';
export const DEV_SIM_FILE = "Use the simulator's sample installer file";
