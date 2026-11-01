#!/usr/bin/env python3
"""Check the source comments against the code they document.

The compiler sources carry JALv2-era header blocks of the form

    /*
     * NAME
     *   some_function
     *
     * DESCRIPTION
     *   ...
     *
     * PARAMETERS
     *   pf : pfile handle
     *
     * RETURN
     *   ...
     *
     * NOTES
     *   ...
     */

Those blocks rot: the function gets renamed, gains a parameter, moves
further down the file, or the block is left behind when a new helper is
inserted under it.  A stale block is worse than no block -- it is read as
fact.  This check fails on the four mechanical shapes:

  1. the named function does not exist anywhere in src/
  2. the function directly following the block is not the named one
  3. PARAMETERS names do not match the function's parameters
  4. RETURN is present on a void function, or missing on a value-returning
     one (a void function's null return is the signature, not documentation)

It also checks the file header line (`** file.c : ...`) against the real
file name.  Nothing here needs the compiler to build, so it runs in the
compile-pin gate (unit-tests/run_compile_pin_tests.sh).

A NAME line may be a family wildcard (`allow_*`): such a block documents
several functions and is exempt from checks 1 and 2.

Usage: check_comment_docs.py [--src DIR] [--verbose]
Exit:  0 = clean, 1 = findings.
"""
import argparse
import os
import re
import sys

BLOCK = re.compile(r'/\*(?:(?!\*/).)*?\bNAME\b(?:(?!\*/).)*?\*/', re.S)
NAME = re.compile(r'\bNAME\b[ \t]*\n[ \t]*\*[ \t]+([A-Za-z_]\w*)(\*?)')
SECTION = re.compile(r'^ \* (NAME|DESCRIPTION|PARAMETERS|RETURN|NOTES)\s*$')
FUNCDEF = re.compile(
    r'^[ \t]*(?:static[ \t]+|extern[ \t]+)?'
    r'([A-Za-z_][\w \t\*]*?)\b([A-Za-z_]\w*)\s*\(([^;{]*?)\)\s*([{;])',
    re.M | re.S)
HEADER = re.compile(r'\*\*\s*([A-Za-z0-9_]+\.(?:c|h))\s*:')
STAR = '*'


def is_blank(line):
    return (not line.strip()) or line.strip() == STAR


def parse_sections(block):
    """Return [(section, body_lines)] for a NAME block."""
    body = block[2:block.rfind('*/')]
    lines = body.split('\n')
    if lines and is_blank(lines[0]):
        lines = lines[1:]
    out = []
    cur = None
    for line in lines:
        m = SECTION.match(line)
        if m:
            cur = [m.group(1), []]
            out.append(cur)
        elif cur is not None:
            cur[1].append(line)
    return out


def section_body(secs, key):
    for name, body in secs:
        if name == key:
            return body
    return None


def param_names(doc_body):
    """Identifiers of a PARAMETERS section (``name : text`` lines)."""
    names = []
    for line in doc_body:
        m = re.match(r'^[ \t]*\*[ \t]+([A-Za-z_]\w*)[ \t]*:', line)
        if m:
            names.append(m.group(1))
    return names


def signature_params(text):
    names = []
    text = text.strip()
    if (not text) or (text == 'void'):
        return names
    depth = 0
    cur = ''
    parts = []
    for ch in text:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += ch
    parts.append(cur)
    for part in parts:
        part = re.sub(r'\[[^\]]*\]', '', part)
        # a function-pointer parameter: the name is inside (*name)
        fp = re.search(r'\(\s*\*\s*([A-Za-z_]\w*)\s*\)', part)
        if fp:
            names.append(fp.group(1))
            continue
        toks = re.findall(r'[A-Za-z_]\w*', part)
        if toks:
            names.append(toks[-1])
    return names


def collect(files):
    """Map function name -> definition offset for every file."""
    defs = {}
    texts = {}
    for path in files:
        text = open(path, errors='replace').read()
        texts[path] = text
        for m in FUNCDEF.finditer(text):
            rtype, name, params, end = m.group(1, 2, 3, 4)
            if end == ';':
                continue
            defs.setdefault(name, []).append(path)
    return texts, defs


def check_file(path, text, defs, findings, verbose):
    base = os.path.basename(path)
    head = text[:1200]
    m = HEADER.search(head)
    if m and m.group(1) != base:
        findings.append('%s:1: header names %s, file is %s'
                        % (path, m.group(1), base))

    blocks = list(BLOCK.finditer(text))
    seen = set()
    for idx, bm in enumerate(blocks):
        block = bm.group(0)
        line = text[:bm.start()].count('\n') + 1
        nm = NAME.search(block)
        if not nm:
            continue
        name = nm.group(1)
        wildcard = nm.group(2) == STAR
        secs = parse_sections(block)
        # a block is one comment: every body line must carry the ' *' prefix.
        # A line that does not is either a broken edit or code smuggled into
        # the block, and the section parser cannot see it.
        for off, body_line in enumerate(block.split('\n')[1:-1], 1):
            if not body_line.startswith(' *'):
                findings.append('%s:%d: %s has a block line without the "*" '
                                'prefix (%r)' % (path, line + off, name,
                                                 body_line[:40]))
                break
        verbose and print('%s:%d: %s' % (path, line, name))

        if name in seen:
            findings.append('%s:%d: %s is documented twice in this file'
                            % (path, line, name))
        seen.add(name)
        if not wildcard:
            if name not in defs:
                findings.append('%s:%d: documents %s, which does not exist'
                                % (path, line, name))
                continue
            # the next function after the block must be the named one
            nxt = FUNCDEF.search(text, bm.end())
            nxt = next((m for m in FUNCDEF.finditer(text, bm.end())
                        if m.group(4) == '{'), None)
            if nxt and nxt.group(2) != name:
                findings.append(
                    '%s:%d: documents %s, but the next function is %s (line %d)'
                    % (path, line, name, nxt.group(2),
                       text[:nxt.start()].count('\n') + 1))

        # signature of the documented function (first definition in this file)
        sig = None
        for m in FUNCDEF.finditer(text):
            if m.group(2) == name and m.group(4) == '{':
                sig = m
                break
        if sig is None and not wildcard:
            sig = None
        if sig is not None:
            rtype, params = sig.group(1).strip(), sig.group(3)
            want = signature_params(params)
            got = section_body(secs, 'PARAMETERS')
            if got is not None:
                have = param_names(got)
                if not have:
                    findings.append('%s:%d: %s has an empty PARAMETERS section'
                                    % (path, line, name))
                elif sorted(have) != sorted(want):
                    findings.append('%s:%d: %s PARAMETERS %s != signature %s'
                                    % (path, line, name, have, want))
            ret = section_body(secs, 'RETURN')
            void = (rtype == 'void')
            ret_text = ' '.join(l.strip(' *') for l in (ret or [])).strip()
            if ret is not None and not ret_text:
                findings.append('%s:%d: %s has an empty RETURN section'
                                % (path, line, name))
            elif ret is not None and void:
                findings.append('%s:%d: %s returns nothing but has a RETURN '
                                'section ("%s")' % (path, line, name, ret_text))
            elif ret is not None and (not void) and ret_text.lower() == 'none':
                findings.append('%s:%d: %s returns %s but its RETURN says '
                                '"none"' % (path, line, name, rtype))
            elif ret is None and not void:
                findings.append('%s:%d: %s returns %s but has no RETURN '
                                'section' % (path, line, name, rtype))


def main():
    ap = argparse.ArgumentParser()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument('--src', default=os.path.join(root, 'src'))
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    files = []
    for dirpath, _, names in os.walk(args.src):
        for name in sorted(names):
            if name.endswith(('.c', '.h')):
                files.append(os.path.join(dirpath, name))
    files.sort()
    texts, defs = collect(files)
    findings = []
    for path in files:
        check_file(path, texts[path], defs, findings, args.verbose)

    for f in findings:
        print(f)
    print('%d file(s), %d doc block finding(s)'
          % (len(files), len(findings)))
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
