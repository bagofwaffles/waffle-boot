// SPDX-License-Identifier: GPL-2.0-or-later
//
// The installer's numbers and words: its request types, the fields of its replies, and every
// sentence the page says about them. install.js imports this file and exports its names again.

// The ferret tool's page, for a unit that has never had developer mode on.
export const FERRET_URL = 'https://bagofwaffles.github.io/ferret/op-xy/';

export const INSTALLER_FILE = 'waffle-install.ldr';
// The OP-1F's installer and its survey, laid out like the OP-1F's own OS (S-05.040): written by
// that unit's own means, never sent by this page.
export const INSTALLER_FILE_1F = 'waffle-install-1f.ldr';
export const SURVEY_FILE_1F = 'waffle-install-1f-survey.ldr';

export const IHELLO = 0x40, STATE = 0x41, TEST = 0x42, REPORT = 0x43, SURVEY = 0x44;
export const MAGIC = [0x57, 0x42, 0x49, 0x4E];          // "WBIN"
export const IPROTO = 1;
export const IHELLO_LEN = 64, STATE_HEAD = 32, REPORT_CHUNK = 1024, SURVEY_LEN = 76;
// HELLO's bytes 56 to 59: the unit's device and how it was decided (S-18.005's codes), the device
// the installer is built for, and 1 for a survey build
export const IHELLO_O_DEVICE = 56;
export const DEVICES = ['OP-XY', 'OP-1F'];
export const HOW_TABLE = 0, HOW_SECOND = 1, HOW_UNKNOWN = 2;
export const UNKNOWN = 0xFF;                            // the reply to a type it does not know
export const NONE = 0xFF;

// flags
export const F_CHECKS_DONE = 1, F_UNLOCKED = 2, F_IMAGE_OK = 4, F_BREADCRUMB = 8, F_SURVEY = 16;

// slot state bits
export const SB_GOOD = 0x01, SB_TRIAL_IMAGE = 0x02, SB_COMMITTED = 0x04, SB_PENDING = 0x08,
    SB_STARTED = 0x10, SB_SPENT = 0x20, SB_ODD = 0x40, SB_REACHED = 0x80;

export const STARTING = 0, CHECKING = 1, GUIDED = 2, LOCKED = 3, READY = 4, WRITING_TRIAL = 5,
    TRIAL_WRITTEN = 6, COMMIT_READY = 7, COMMITTING = 8, DONE = 9, FAILED = 10, TRIAL_FAILED = 11,
    LEFT = 12, SURVEYED = 13;

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
    13: ['surveyed', 'The survey is done: nothing was written to the SPI flash'],
};

export const VERDICT = { 0: 'PASS', 1: 'PASS WITH GAPS', 2: 'FAIL', 0xFF: 'not yet' };
export const TEST_STATE = ['not run', 'pass', 'FAIL', 'not tested', 'not passed', 'n/a'];
export const TEST_KIND = ['OS', 'automatic', 'guided'];

export const LOCK = {
    1: 'a check failed',
    2: 'a check was not tested',
    3: 'the installer did not verify the image it carries (CRC-32 or front pin)',
    4: 'the slot the selector names is not verified or not good',
    5: "the unit's OS was not found, or fails its check",
    6: 'the key check was ended with 8',
    7: 'the eMMC could not be reached',
    8: 'the device is not identified by its first stage',
    9: 'this installer is built for another device',
    10: "the trial would write over the unit's own bootloader, the last one it has",
    11: 'this is a survey build: it writes nothing to the SPI flash',
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
    8: "The unit's first stage is not one this project knows: nothing is installed on it. " +
        'check.txt gives its n and c.',
    9: `Use the installer built for this unit: ${INSTALLER_FILE} on an OP-XY, ` +
        `${INSTALLER_FILE_1F} on an OP-1F.`,
    10: 'On an OP-1F one slot keeps the bootloader the unit came with; this install would write ' +
        'over the last one, so it is not offered.',
    11: 'Save check.txt: it holds what the survey found. An install is not offered by this build.',
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
    0x45: 'refused: a selector step failed and was left as it was; restart the unit first',
    0x46: "trial refused: it would write over the unit's own bootloader",
    0x47: 'refused: a survey build writes nothing to the SPI flash',
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

export const SURVEY_DONE = 'The survey wrote nothing to the SPI flash. Save check.txt below: it ' +
    'holds what an install on this unit would depend on.';
export const NOT_IDENTIFIED = 'The device is not identified by its first stage: nothing is ' +
    'installed on it.';

export const NAMES = ['Slot 1', 'Slot 2'];
export const deviceName = (d) => DEVICES[d] || `device ${d}`;
export const slotName = (k) => (k === 0 || k === 1 ? NAMES[k] : 'no slot');
export const testState = (s) => TEST_STATE[s] || `state ${s}`;
export const lockText = (r) => LOCK[r] || `lock reason ${r}`;
export const osText = (o) => OS_PART[o] || `state ${o}`;
export const trialText = (t) => TRIAL[t] || `state ${t}`;
export const hex8 = (n) => (n >>> 0).toString(16).padStart(8, '0');
export const failureText = (c) => FAILURE[c] || `failure code 0x${c.toString(16).padStart(2, '0')}`;
export const slotBits = (b) => SLOT_BITS.filter(([m]) => b & m).map(([, w]) => w);
