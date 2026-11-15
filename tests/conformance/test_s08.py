# SPDX-License-Identifier: GPL-2.0-or-later
"""S-08 reading rules: src/s08_rules.c (through h_s08) against tests/oracle/S-08-nordic/vectors."""

import json
import unittest

from harness import hx, requirement, run, spec_path

MODE = {None: '0', 'dfu': '1', 'not': '2'}        # S08_NO_ANSWER, S08_IN_DFU, S08_NOT_IN_DFU
DFU = {'fail': '0', 'ok': '1', 'busy': '2'}


def vectors(name):
    with open(spec_path('S-08-nordic', 'vectors', name), encoding='utf-8') as f:
        return json.load(f)


def text(reply):
    return 'none' if reply is None else hx(reply.encode())


def num(v):
    return v if isinstance(v, int) else int(v, 0)


class Greeting(unittest.TestCase):

    @requirement('S-08.010', 'S-08.011', 'S-08.012')
    def test_S08_011_answers(self):
        g = vectors('greeting.json')
        rows = g['rows'] + g['sequence']
        out = run('s08', ['greet %s %s' % ('unknown' if r['state'] is None else num(r['state']),
                                           r['payload'] or '-') for r in rows])
        for r, o in zip(rows, out):
            a = r['answer']
            with self.subTest(state=r['state'], payload=r['payload']):
                self.assertEqual(o, '-' if a is None else '0x%x 0x%x' % (num(a['id']), int(a['payload'], 16)))

    @requirement('S-08.031', 'S-08.033')
    def test_S08_031_counts_as_greeting(self):
        rows = vectors('greeting.json')['accept']
        out = run('s08', ['accept %s %s' % (r['id'], r['payload'] or '-') for r in rows])
        for r, o in zip(rows, out):
            with self.subTest(id=r['id'], payload=r['payload']):
                self.assertEqual(o, str(int(r['counts'])))


class Queries(unittest.TestCase):

    @requirement('S-08.020', 'S-08.021')
    def test_S08_021_mode(self):
        rows = vectors('queries.json')['mode']
        out = run('s08', ['mode ' + text(r['reply']) for r in rows])
        for r, o in zip(rows, out):
            with self.subTest(reply=r['reply']):
                self.assertEqual(o, MODE[r['expected']])

    @requirement('S-08.022')
    def test_S08_022_boot_slot(self):
        rows = vectors('queries.json')['boot_slot']
        out = run('s08', ['slot ' + text(r['reply']) for r in rows])
        for r, o in zip(rows, out):
            with self.subTest(reply=r['reply']):
                if r['expected'] == 'other':            # any slot other than 0 and 1, or none (S-08.022)
                    self.assertNotIn(int(o), (0, 1))
                else:
                    self.assertEqual(int(o), -1 if r['expected'] is None else r['expected'])


class Dfu(unittest.TestCase):

    @requirement('S-08.040')
    def test_S08_040_reply_classes(self):
        rows = vectors('dfu_replies.json')['rows']
        out = run('s08', ['dfu %d %s %s' % (r['command'], r['reply_id'], r['payload'] or '-') for r in rows])
        for r, o in zip(rows, out):
            with self.subTest(row=r):
                self.assertEqual(o, DFU[r['class']])


class Transfer(unittest.TestCase):

    @requirement('S-08.045')
    def test_S08_045_product_word(self):
        rows = vectors('transfers.json')['product_word']
        out = run('s08', ['product %d %d' % (r['sku'], r['read_ok']) for r in rows])
        for r, o in zip(rows, out):
            with self.subTest(row=r):
                self.assertEqual(int(o, 16), int(r['word'], 16))

    @requirement('S-08.043')
    def test_S08_043_prepare(self):
        rows = vectors('transfers.json')['prepare']
        out = run('s08', ['prepare %s %s %s' % (' '.join(map(str, r['version'])), r['type'], r['product_word'])
                          for r in rows])
        for r, o in zip(rows, out):
            with self.subTest(version=r['version']):
                self.assertEqual(o, r['payload'])

    @requirement('S-08.043', 'S-08.045')
    def test_S08_045_product_word_power_ons(self):
        """product_word_power_ons: each power-on's product word from the sku it booted with, and the
        prepare (version 1.1.0.0, type B0h) carrying it. That the word is read once, at start-up, and
        not when the transfer runs needs the run (SC-08-TRANSFER), not driven here."""
        rows = vectors('transfers.json')['product_word_power_ons']
        out = run('s08', [c for r in rows for c in ('product %d 1' % r['boot_sku'],
                                                    'prepare 1 1 0 0 0xb0 %s' % r['word'])])
        for r, word, prepare in zip(rows, out[0::2], out[1::2]):
            with self.subTest(power_on=r['power_on']):
                self.assertEqual(int(word, 16), int(r['word'], 16))
                self.assertEqual(prepare, r['prepare'])

    @requirement('S-08.044')
    def test_S08_044_header(self):
        rows = vectors('transfers.json')['header']
        out = run('s08', ['header %d %s %s' % (r['image_len'], ' '.join(map(str, r['version'])),
                                               r['product_word']) for r in rows])
        for r, o in zip(rows, out):
            with self.subTest(image_len=r['image_len']):
                self.assertEqual(o, r['header'])


if __name__ == '__main__':
    unittest.main()
