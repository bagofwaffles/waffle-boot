#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks the links of wiki/ and the tree's pointers into it. Python 3, standard library only.

    test_wiki_links.py

- Every link between wiki pages names a page that exists, and an anchor names one of its headings.
- Every picture or repository file a page names (../docs/ui/..., ../blob/main/...) exists.
- Every page can be reached from Home or the sidebar.
- In the tree, every wiki address (.../waffle-boot/wiki/Page), every wiki/Page.md and every
  docs/*.md it names exists. src/front/ is not read (its comments change only with the front), nor
  the leak check's tests, whose paths are examples.
"""
import os
import re
import sys
import unittest

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WIKI = os.path.join(TOP, 'wiki')
SITE = 'https://bagofwaffles.github.io/waffle-boot/'
WIKI_URL = 'https://github.com/bagofwaffles/waffle-boot/wiki/'

MD_LINK = re.compile(r'!?\[[^\]]*\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\s*\)')
HTML_LINK = re.compile(r'\b(?:src|href|srcset)="([^"]+)"')
HEADING = re.compile(r'^(#{1,6})\s+(.*?)\s*#*\s*$')
FENCE = re.compile(r'^\s*(```|~~~)')


def slug(heading):
    """The anchor GitHub gives a heading."""
    s = re.sub(r'`|\*|_(?=\W)|(?<=\W)_', '', heading.strip().lower())
    s = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', s)
    s = re.sub(r'[^\w\- ]', '', s)
    return s.replace(' ', '-')


def strip_code(text):
    """The page with fenced blocks and inline code taken out, so example text is not read as links."""
    out, fenced = [], False
    for line in text.splitlines():
        if FENCE.match(line):
            fenced = not fenced
            continue
        if not fenced:
            out.append(re.sub(r'`[^`]*`', '', line))
    return '\n'.join(out)


def anchors(text):
    seen, out = {}, set()
    fenced = False
    for line in text.splitlines():
        if FENCE.match(line):
            fenced = not fenced
            continue
        m = None if fenced else HEADING.match(line)
        if m:
            a = slug(m.group(2))
            n = seen.get(a, 0)
            seen[a] = n + 1
            out.add(a if n == 0 else '%s-%d' % (a, n))
    for m in re.finditer(r'<a\s+(?:id|name)="([^"]+)"', text):
        out.add(m.group(1))
    return out


def pages(wiki=WIKI):
    out = {}
    for f in sorted(os.listdir(wiki)):
        if f.endswith('.md'):
            with open(os.path.join(wiki, f), encoding='utf-8') as fh:
                out[f[:-3]] = fh.read()
    return out


def links(text):
    body = strip_code(text)
    out = [m.group(1) for m in MD_LINK.finditer(body)]
    for m in HTML_LINK.finditer(body):
        out += [part.split()[0] for part in m.group(1).split(',') if part.strip()]
    return out


def site_file(path, top):
    path = path.split('#')[0].split('?')[0]
    full = os.path.join(top, 'docs', path)
    if path == '' or path.endswith('/'):
        full = os.path.join(full, 'index.html')
    return os.path.isfile(full)


def check_wiki(wiki=WIKI, top=TOP):
    """Every fault in wiki/, as text; an empty list when there is none."""
    ps = pages(wiki)
    heads = {name: anchors(text) for name, text in ps.items()}
    faults, edges = [], {name: set() for name in ps}
    for name, text in ps.items():
        for target in links(text):
            where = '%s: %s' % (name, target)
            if target.startswith(('mailto:',)):
                continue
            if target.startswith(WIKI_URL):
                target = target[len(WIKI_URL):]
            elif target.startswith(SITE):
                if not site_file(target[len(SITE):], top):
                    faults.append(where + ': no such file on the site')
                continue
            elif re.match(r'[a-z]+:', target):
                continue
            if target.startswith('#'):
                if target[1:] not in heads[name]:
                    faults.append(where + ': no such heading')
                continue
            if target.startswith('../'):
                rel = target[3:].split('#')[0]
                m = re.match(r'(?:blob|tree)/main/(.*)', rel)
                rel = m.group(1) if m else rel
                if not os.path.exists(os.path.join(top, rel)):
                    faults.append(where + ': no such file in the repository')
                elif re.match(r'docs/[^/]+\.md$', rel):
                    faults.append(where + ': sends the reader to a Markdown file, not a wiki page')
                continue
            page, _, anchor = target.partition('#')
            if page not in ps:
                faults.append(where + ': no such page')
                continue
            edges[name].add(page)
            if anchor and anchor not in heads[page]:
                faults.append(where + ': no such heading on ' + page)
    roots = [n for n in ('Home', '_Sidebar') if n in ps]
    seen, todo = set(roots), list(roots)
    while todo:
        for nxt in edges[todo.pop()]:
            if nxt not in seen:
                seen.add(nxt)
                todo.append(nxt)
    for name in ps:
        if name not in seen and name != '_Footer':
            faults.append('%s: reached from neither Home nor the sidebar' % name)
    return faults


SKIP_DIRS = {'.git', 'third_party', 'build', 'spec', 'bench', 'handoff', '.adi', '.wt', 'node_modules'}
TREE_WIKI = re.compile(re.escape(WIKI_URL) + r'([A-Za-z0-9_-]+)(#[\w-]+)?')
TREE_WIKI_PATH = re.compile(r'(?<![\w/.])wiki/([A-Za-z0-9_-]+)\.md')
TREE_DOCS = re.compile(r'(?<![\w/.])docs/([\w-]+)\.md')


def tree_files(top=TOP):
    for d, dirs, files in os.walk(top):
        rel = os.path.relpath(d, top)
        dirs[:] = sorted(x for x in dirs if x not in SKIP_DIRS and not (rel == '.' and x.startswith('.')
                                                                         and x != '.github'))
        if rel.replace(os.sep, '/').startswith('src/front'):
            continue
        for f in sorted(files):
            if f.endswith(('.c', '.h', '.S', '.inc', '.ld', '.py', '.sh', '.md', '.mjs', '.js', '.html',
                           '.yml', '.txt', '.json')) or f in ('Makefile', 'Dockerfile'):
                yield os.path.join(d, f)


def check_tree(top=TOP):
    """Every pointer into the wiki or docs/*.md, outside wiki/ and src/front/, that names nothing."""
    ps = pages(os.path.join(top, 'wiki'))
    heads = {name: anchors(text) for name, text in ps.items()}
    faults = []
    for path in tree_files(top):
        rel = os.path.relpath(path, top).replace(os.sep, '/')
        if rel.startswith('wiki/') or rel in ('tests/tools/test_wiki_links.py', 'tests/tools/test_leak_check.py'):
            continue
        try:
            with open(path, encoding='utf-8') as f:
                text = f.read()
        except (UnicodeDecodeError, OSError):
            continue
        for m in TREE_WIKI.finditer(text):
            page, anchor = m.group(1), (m.group(2) or '#')[1:]
            if page not in ps:
                faults.append('%s: wiki page %s does not exist' % (rel, page))
            elif anchor and anchor not in heads[page]:
                faults.append('%s: wiki page %s has no heading #%s' % (rel, page, anchor))
        for m in TREE_WIKI_PATH.finditer(text):
            if m.group(1) not in ps:
                faults.append('%s: wiki/%s.md does not exist' % (rel, m.group(1)))
        for m in TREE_DOCS.finditer(text):
            if not os.path.isfile(os.path.join(top, 'docs', m.group(1) + '.md')):
                faults.append('%s: docs/%s.md does not exist' % (rel, m.group(1)))
    return faults


class TestSlug(unittest.TestCase):
    def test_github_anchors(self):
        self.assertEqual(slug('The slot menu (key 1)'), 'the-slot-menu-key-1')
        self.assertEqual(slug('1. System information and logs'), '1-system-information-and-logs')
        self.assertEqual(slug('`--force`'), '--force')
        self.assertEqual(slug("The installer's USB protocol"), 'the-installers-usb-protocol')

    def test_repeated_headings(self):
        self.assertEqual(anchors('## Steps\n## Steps\n'), {'steps', 'steps-1'})

    def test_code_is_not_a_link(self):
        self.assertEqual(links('see `[a](Nowhere)`\n```\n[b](Nowhere)\n```\n[c](Here)\n'), ['Here'])


class TestOnTempWiki(unittest.TestCase):
    def setUp(self):
        import tempfile
        self.top = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.top, 'wiki'))
        os.makedirs(os.path.join(self.top, 'docs', 'ui'))
        open(os.path.join(self.top, 'docs', 'ui', 'a.png'), 'wb').close()

    def tearDown(self):
        import shutil
        shutil.rmtree(self.top)

    def put(self, name, text):
        with open(os.path.join(self.top, 'wiki', name + '.md'), 'w', encoding='utf-8') as f:
            f.write(text)

    def faults(self):
        return check_wiki(os.path.join(self.top, 'wiki'), self.top)

    def test_clean(self):
        self.put('Home', '[A](Page-A) ![x](../docs/ui/a.png)\n')
        self.put('Page-A', '## Part\n[back](Home) [self](#part)\n')
        self.assertEqual(self.faults(), [])

    def test_faults(self):
        self.put('Home', '[A](Page-A#nope) [B](Page-B) ![x](../docs/ui/b.png) [m](../blob/main/docs/x.md)\n')
        self.put('Page-A', '## Part\n')
        self.put('Lost', 'nobody links here\n')
        f = '\n'.join(self.faults())
        self.assertIn('Home: Page-A#nope: no such heading on Page-A', f)
        self.assertIn('Home: Page-B: no such page', f)
        self.assertIn('Home: ../docs/ui/b.png: no such file', f)
        self.assertIn('Home: ../blob/main/docs/x.md: no such file', f)
        self.assertIn('Lost: reached from neither Home nor the sidebar', f)


class TestThisTree(unittest.TestCase):
    def test_wiki(self):
        faults = check_wiki()
        self.assertEqual(faults, [], '\n' + '\n'.join(faults))

    def test_tree(self):
        faults = check_tree()
        self.assertEqual(faults, [], '\n' + '\n'.join(faults))


if __name__ == '__main__':
    unittest.main()
