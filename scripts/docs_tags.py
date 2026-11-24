#!/usr/bin/env python3
"""Find docs in the docs/ bundle by the tags in their YAML headers.

Usage:
  docs_tags.py                       every tag with its number of docs, most used first
  docs_tags.py TAG [TAG ...]         docs that have ALL these tags
  docs_tags.py --any TAG [TAG ...]   docs that have ANY of these tags
  docs_tags.py --type Result TAG     only docs of this type (Concept, How-To, Site, Reference, Result)
  docs_tags.py --show PATH           the type, title, tags and description of one doc
  docs_tags.py --untagged            docs with a header but no tags

Each match prints: path, type, title. Add -d to also print the description.
Run from anywhere; the bundle is found relative to this script. Standard library only.
"""
import argparse
import os
import re
import sys
from collections import Counter

DOCS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs'))


def header(path):
    with open(path, encoding='utf-8') as fh:
        text = fh.read()
    if not text.startswith('---'):
        return None
    head = text.split('---', 2)[1]
    field = lambda k: (m.group(1) if (m := re.search(r'^%s:\s*"?(.*?)"?\s*$' % k, head, re.M)) else '')
    tags = re.search(r'^tags:\s*\[(.*)\]', head, re.M)
    return dict(path=os.path.relpath(path, os.path.dirname(DOCS)), type=field('type'), title=field('title'),
                description=field('description'), status=field('status'),
                tags=[t.strip() for t in tags.group(1).split(',') if t.strip()] if tags else [])


def bundle():
    for root, _, files in os.walk(DOCS):
        for f in sorted(files):
            if f.endswith('.md') and f not in ('index.md', 'log.md'):
                h = header(os.path.join(root, f))
                if h:
                    yield h


def show(d, desc):
    status = f" [{d['status']}]" if d['status'] else ''
    print(f"{d['path']}  ({d['type']}{status})  {d['title']}")
    if desc:
        print(f"    tags: {', '.join(d['tags'])}\n    {d['description']}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('tags', nargs='*')
    p.add_argument('--any', action='store_true', help='match any tag instead of all')
    p.add_argument('--type', help='only this doc type')
    p.add_argument('--show', metavar='PATH', help='print one doc header')
    p.add_argument('--untagged', action='store_true')
    p.add_argument('-d', action='store_true', help='also print tags and description')
    a = p.parse_args()
    docs = list(bundle())
    if a.show:
        for d in docs:
            if d['path'].endswith(a.show):
                show(d, True)
        return
    if a.untagged:
        for d in docs:
            if not d['tags']:
                show(d, a.d)
        return
    if a.type:
        docs = [d for d in docs if d['type'].lower() == a.type.lower()]
    if not a.tags:
        for tag, n in sorted(Counter(t for d in docs for t in d['tags']).items(), key=lambda x: (-x[1], x[0])):
            print(f'{n:4}  {tag}')
        return
    want = [t.lower() for t in a.tags]
    test = any if a.any else all
    hits = [d for d in docs if test(t in [x.lower() for x in d['tags']] for t in want)]
    for d in sorted(hits, key=lambda d: d['path']):
        show(d, a.d)
    if not hits:
        sys.exit(f"no doc has {'any' if a.any else 'all'} of: {', '.join(a.tags)} (run with no arguments for every tag)")


if __name__ == '__main__':
    main()
