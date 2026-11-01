#!/usr/bin/env python3
"""
Ownership scan for `value_t` parameters (review aid, not a gate).

`value_t` is a handle with a release contract: an API that hands one out
transfers a reference, and the receiver releases it. A function that takes a
`value_t` BY VALUE and releases that same parameter CONSUMES its argument --
and nothing in the signature says so. Two functions disagreeing about that is
how `pic_eeprom_or_id_dump()` (consumed) vs `pic_cmd_dump()` (released again)
produced a double release that hid for years behind `cache_element_seek()`
returning NULL for freed elements.

  unconditional  the first release runs at function-body depth with no
                 earlier return -- the "consume" convention, safe while the
                 caller knows
  conditional    the release sits inside a branch/loop, or an earlier return
                 can leave without consuming -- the caller cannot know whether
                 it still owns the value

LIMITS: brace/depth heuristic, not a parser. No goto, no macro expansion, and
it cannot tell that both arms of an if may release. Read the conditional list;
do not treat it as a defect list.
"""
import glob
import os
import re
import sys


def functions_of(text):
    lines = text.split('\n')
    depth = 0
    sig = []
    cur = None
    out = []
    for i, line in enumerate(lines):
        opens, closes = line.count('{'), line.count('}')
        if depth == 0:
            if opens > closes:
                sig.append(line)
                cur = {'line': i + 2 - len(sig), 'header': '\n'.join(sig),
                       'body': [line]}
                sig = []
            elif (line.strip().endswith(';') or line.strip().endswith('}')
                  or not line.strip()):
                sig = []
            else:
                sig.append(line)
        elif cur is not None:
            cur['body'].append(line)
        depth += opens - closes
        if depth <= 0:
            depth = 0
            if cur is not None:
                out.append((cur['line'], cur['header'], '\n'.join(cur['body'])))
                cur = None
    return out


def parse_signature(header):
    text = header
    cut = text.rfind('*/')
    if cut >= 0:
        text = text[cut + 2:]
    m = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(([^()]*)\)\s*\{?\s*$', text, re.S)
    if not m:
        return None, []
    return m.group(1), re.findall(r'value_t\s+([A-Za-z_][A-Za-z0-9_]*)', m.group(2))


def scan(root):
    out = []
    for path in sorted(glob.glob(os.path.join(root, '**', '*.c'), recursive=True)):
        for line, header, body in functions_of(open(path, errors='replace').read()):
            name, params = parse_signature(header)
            if not name:
                continue
            for prm in params:
                hits = []
                d = 0
                for j, bl in enumerate(body.split('\n')):
                    d += bl.count('{') - bl.count('}')
                    if re.search(r'value_release\s*\(\s*' + re.escape(prm) + r'\s*\)', bl):
                        hits.append((j, d))
                if not hits:
                    continue
                # a parameter REASSIGNED before the release is no longer the
                # argument -- releasing it releases an internal value (the
                # `f = pic_indirect_get(...)` pattern). Only report releases
                # of the parameter as received.
                body_lines = body.split('\n')
                first = hits[0][0]
                if any(re.search(r'(?<![=!<>])\b' + re.escape(prm) + r'\s*=(?!=)', l)
                       for l in body_lines[:first]):
                    continue
                early = any(re.search(r'\breturn\b', l)
                            for l in body.split('\n')[:hits[0][0]])
                kind = 'conditional'
                if hits[0][1] <= 1 and not early and len(hits) == 1:
                    kind = 'unconditional'
                out.append((path, line, name, prm, kind, len(hits)))
    return out


def main(argv):
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src')
    findings = scan(root)
    cond = [f for f in findings if f[4] == 'conditional']
    uncond = [f for f in findings if f[4] == 'unconditional']
    print("functions taking a value_t by value that release that parameter: %d"
          % len(findings))
    print("  unconditional consume: %d" % len(uncond))
    print("  conditional consume:   %d   <- read these" % len(cond))
    if '--cond' in argv or '--list' in argv:
        for path, line, name, prm, kind, n in (cond if '--cond' in argv else findings):
            rel = os.path.relpath(path, os.path.join(root, '..'))
            print("    %-11s %s:%d  %s(%s)%s" % (kind, rel, line, name, prm,
                  '' if n == 1 else '  [%d releases]' % n))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
