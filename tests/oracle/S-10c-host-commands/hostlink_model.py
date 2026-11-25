# SPDX-License-Identifier: GPL-2.0-or-later
"""S-10c.123, S-10c.127, S-10c.131, S-10c.135 - S-10c.137: the host link's classes and its approval, as a model
independent of src/core/host/hostlink.c. gen_hostlink.py builds vectors/hostlink_access.json from it."""

SAFE, NEEDS, NEVER, BY_ROOT, HOLD, WARNED = 'safe', 'needs', 'never', 'by root', 'hold', 'warned'

CALLS = {
    0x00: ('HELLO', SAFE), 0x01: ('SESSION', SAFE), 0x02: ('APPROVE', SAFE), 0x03: ('END', SAFE),
    0x04: ('INFO', SAFE), 0x05: ('SET', NEEDS), 0x06: ('JOB', SAFE),
    0x10: ('STAT', BY_ROOT), 0x11: ('LIST', BY_ROOT), 0x12: ('GET', NEEDS), 0x13: ('HASH', NEEDS),
    0x14: ('PUT_OPEN', NEEDS), 0x15: ('PUT_DATA', NEEDS), 0x16: ('PUT_END', NEEDS),
    0x17: ('REMOVE', NEEDS), 0x18: ('MKDIR', NEEDS), 0x19: ('RENAME', NEEDS),
    0x20: ('RUN', None),                                    # its action's class
    0x21: ('RAM_OPEN', NEEDS), 0x22: ('RAM_DATA', NEEDS), 0x23: ('RAM_END', NEEDS),   # S-10c.135
    # S-10c.131: the PSoC calls ask for no approval; the unit warns and counts down instead, in every
    # session, the Developer item's included
    0x30: ('PSOC_OPEN', WARNED), 0x31: ('PSOC_DATA', WARNED), 0x32: ('PSOC_CLOSE', WARNED),
    0x33: ('PSOC_STATE', SAFE), 0x34: ('PSOC_RESET', WARNED),
}

# S-10c.137: an install can write the SPI flash, so it takes the hold whatever Developer's item says
ACTIONS = {
    0x01: ('install', HOLD), 0x02: ('OS write', NEEDS), 0x03: ('start once', NEEDS),
    0x04: ('restart', NEEDS), 0x05: ('continue to OS', NEEDS), 0x06: ('restart into a mode', NEEDS),
    0x07: ('RAM start', NEEDS),                             # S-10c.136
    0x10: ('factory reset', NEVER), 0x11: ('Protected writes', NEVER),
}

# The session states (HELLO, SESSION); 5 is S-10c.137's
SESSIONS = {0: 'none', 1: 'pending', 2: 'approved', 3: 'refused', 4: 'timed out', 5: 'approved, no hold'}

OPEN_ROOTS = ('yaffs2',)                                    # list and stat with no approval


def call_class(call, path_root=None, action=None):
    """The class a request takes: by its root for STAT and LIST, by its action for RUN."""
    name, cls = CALLS[call]
    if cls == BY_ROOT:
        return SAFE if path_root in OPEN_ROOTS else NEEDS
    if cls is None:
        return ACTIONS[action][1]
    return cls


def gate(session, cls):
    """What the approval makes of a request of class cls in a session: 'never' (status 3),
    'approval' (status 2), or 'past' (the call goes on to its own checks)."""
    if cls == NEVER:
        return 'never'
    if cls in (SAFE, WARNED):
        return 'past'
    if cls == HOLD:
        return 'past' if session == 'approved' else 'approval'
    return 'past' if session in ('approved', 'approved, no hold') else 'approval'


def approve(session, no_hold):
    """APPROVE's outcome: with Developer's item on, a session that is not approved is allowed at once;
    otherwise the screen (pending), and an approved session stays as it is."""
    if session in ('approved', 'pending'):
        return session
    if session != 'approved, no hold' and no_hold:
        return 'approved, no hold'
    return 'pending'


def poll(session, no_hold):
    """Once a pass: the item off ends a session it allowed; a held one stays."""
    return 'none' if session == 'approved, no hold' and not no_hold else session
