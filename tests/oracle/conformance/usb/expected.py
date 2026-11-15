# SPDX-License-Identifier: GPL-2.0-or-later
"""Declarative expected-descriptor check.

A template holds the values a device must report; fields it leaves out are wildcards. Template shape (every key optional):

    {
      'device': {'idVendor': 0x1234, 'bNumConfigurations': 1, ...},          # device descriptor fields
      'qualifier': {...},
      'strings': {1: 'Maker', 2: 'Product'},                                  # index -> text
      'configuration': {
          'fields': {'bNumInterfaces': 4, 'bmAttributes': 0xC0, ...},
          'interfaces': [                                                     # in wire order
              {'bInterfaceNumber': 0, 'bAlternateSetting': 0, 'bInterfaceClass': 1, ...,
               'endpoints': [{'bEndpointAddress': 0x81, 'bmAttributes': 2, 'wMaxPacketSize': 512}],
               'cs': [b'\\x09\\x24\\x01...'],                                 # exact class-specific bytes
               'iad': {'bFunctionClass': 2, ...}},
              ...],
          'exact_interface_count': True,                                      # no further interfaces
      },
      'bytes': {'configuration': b'...'},                                     # exact wire bytes, if known
    }

match_* functions return a list of mismatch strings; empty means the reply matches the template.
"""
from . import descriptors as D


def _fields(got, want, where):
    out = []
    for k, v in want.items():
        if k not in got:
            out.append(f'{where}: no field {k}')
        elif got[k] != v:
            g = got[k]
            out.append(f'{where}.{k}: got {g:#x}, want {v:#x}' if isinstance(g, int) and isinstance(v, int)
                       else f'{where}.{k}: got {g!r}, want {v!r}')
    return out


def match_device(raw, want):
    return _fields(D.parse_device(raw), want, 'device')


def match_qualifier(raw, want):
    return _fields(D.parse_qualifier(raw), want, 'qualifier')


def match_string(raw, want_text):
    got = D.parse_string(raw)
    return [] if got == want_text else [f'string: got {got!r}, want {want_text!r}']


def match_configuration(data, want, wlength=None):
    cfg = D.parse_configuration(data, wlength)
    out = _fields(cfg.fields or {}, want.get('fields', {}), 'configuration')
    wifs = want.get('interfaces')
    if wifs is not None:
        got = cfg.alternates
        if want.get('exact_interface_count', True) and not cfg.truncated and len(got) != len(wifs):
            out.append(f'configuration: {len(got)} interface descriptors, want {len(wifs)}')
        for i, (a, w) in enumerate(zip(got, wifs)):
            where = f'interface[{i}]'
            out += _fields(a.fields, {k: v for k, v in w.items() if k not in ('endpoints', 'cs', 'iad')}, where)
            if 'iad' in w:
                if w['iad'] is None:
                    if a.iad is not None:
                        out.append(f'{where}: unexpected IAD')
                elif a.iad is None:
                    out.append(f'{where}: no IAD before it')
                else:
                    out += _fields(a.iad, w['iad'], where + '.iad')
            if 'cs' in w:
                got_cs = [d.raw for d in a.cs]
                if got_cs != [bytes(x) for x in w['cs']]:
                    out.append(f'{where}.cs: got {[x.hex() for x in got_cs]}, want {[bytes(x).hex() for x in w["cs"]]}')
            if 'endpoints' in w:
                if len(a.endpoints) != len(w['endpoints']) and a.complete:
                    out.append(f'{where}: {len(a.endpoints)} endpoints, want {len(w["endpoints"])}')
                for k, (e, we) in enumerate(zip(a.endpoints, w['endpoints'])):
                    out += _fields(e.fields, {x: y for x, y in we.items() if x != 'cs'}, f'{where}.ep[{k}]')
                    if 'cs' in we and [d.raw for d in e.cs] != [bytes(x) for x in we['cs']]:
                        out.append(f'{where}.ep[{k}].cs mismatch')
    return out


def match_bytes(got, want, name='reply'):
    """Exact byte comparison with the first differing offset."""
    got, want = bytes(got), bytes(want)
    if got == want:
        return []
    n = next((i for i in range(min(len(got), len(want))) if got[i] != want[i]), min(len(got), len(want)))
    return [f'{name}: {len(got)} bytes vs {len(want)} expected, first difference at {n}']


def template_from_configuration(data):
    """A full template of a parsed configuration (every field), e.g. to pin a known-good reply."""
    cfg = D.parse_configuration(data)
    ifs = []
    for a in cfg.alternates:
        w = dict(a.fields)
        w['cs'] = [d.raw for d in a.cs]
        w['iad'] = dict(a.iad) if a.iad else None
        w['endpoints'] = [dict(e.fields, cs=[d.raw for d in e.cs]) for e in a.endpoints]
        ifs.append(w)
    return dict(fields=dict(cfg.fields), interfaces=ifs, exact_interface_count=True)


def match_all(replies, template):
    """replies: {'device': bytes, 'qualifier': bytes, 'configuration': bytes, 'strings': {idx: bytes}}."""
    out = []
    if 'device' in template:
        out += match_device(replies['device'], template['device'])
    if 'qualifier' in template:
        out += match_qualifier(replies['qualifier'], template['qualifier'])
    if 'configuration' in template:
        out += match_configuration(replies['configuration'], template['configuration'])
    for idx, text in template.get('strings', {}).items():
        raw = replies.get('strings', {}).get(idx)
        out += [f'string {idx}: missing'] if raw is None else match_string(raw, text)
    for k, b in template.get('bytes', {}).items():
        out += match_bytes(replies.get(k, b''), b, k)
    return out
