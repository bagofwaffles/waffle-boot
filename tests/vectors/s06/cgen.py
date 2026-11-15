# SPDX-License-Identifier: GPL-2.0-or-later
"""Emit deterministic C headers of test vectors: byte arrays as {0x..} lists (never long string
literals, which -pedantic-errors rejects) and tables of structs. Shared by the s06-s09 generators."""
import json


class CHeader:
    def __init__(self, guard, source, generator):
        self.guard = guard
        self.source = source
        self.generator = generator
        self.lines = []
        self.n = 0
        self.decls = []

    @staticmethod
    def cstr(s):
        return json.dumps(s)

    def arr(self, stem, data):
        """A static const uint8_t array holding data; returns 'name, length' for a struct row
        (a pointer and a size_t)."""
        data = bytes(data)
        name = 'v_%s_%d' % (stem, self.n)
        self.n += 1
        items = ['0x%02x' % b for b in data] or ['0']
        rows = ['\t' + ', '.join(items[i:i + 16]) + ',' for i in range(0, len(items), 16)]
        self.lines.append('static const uint8_t %s[%d] = {\n%s\n};' % (name, len(items), '\n'.join(rows)))
        return '%s, %d' % (name, len(data))

    def table(self, ctype, name, rows, decl=None):
        if decl:
            self.decls.append(decl)
        self.lines.append('static const %s %s[] = {\n%s\n};' % (ctype, name, '\n'.join('\t{%s},' % r for r in rows)))
        self.lines.append('#define %s_count (sizeof %s / sizeof %s[0])' % (name, name, name))

    def text(self):
        out = ['/* SPDX-License-Identifier: GPL-2.0-or-later */',
               '/* Generated from %s by %s; do not edit. */' % (self.source, self.generator),
               '#ifndef ' + self.guard, '#define ' + self.guard, '',
               '#include <stddef.h>', '#include <stdint.h>', '']
        out += self.decls + [''] + self.lines + ['', '#endif', '']
        return '\n'.join(out)
