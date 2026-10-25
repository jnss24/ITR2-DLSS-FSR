# make_readme_txt.py - writes the plain-text readme for the release zip (ITR2_DLSS_README.txt) from MANUAL.md
# (build.bat, step 5), so Notepad shows it cleanly: headings underlined, tables as aligned columns (or, when too wide,
# one block per row), no Markdown marks (** * ` [](...)), lines wrapped at 100 characters, CRLF, UTF-8 with BOM.
# Usage: python -I make_readme_txt.py <MANUAL.md> <output.txt>
import re
import sys
import textwrap

WIDTH = 100


def inline(t):
    t = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', r'\1 (\2)', t)        # [text](url) -> text (url)
    t = re.sub(r'\*\*(.+?)\*\*', r'\1', t)                       # bold
    t = re.sub(r'(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])', r'\1', t)   # italic
    t = t.replace('`', '')
    t = re.sub(r'§\s*(\d+)', r'section \1', t)                   # "see §4" -> "see section 4"
    return t.strip()


def wrap(text, indent='', hang=None):
    hang = indent if hang is None else hang
    # a number keeps its unit: "67 %" or "5 ms" never splits across lines
    glued = re.sub(r'(\d) (%|ms|MB|GB|KB|Hz|fps|s\b)', '\\1\x00\\2', text)
    lines = textwrap.wrap(glued, width=WIDTH, initial_indent=indent, subsequent_indent=hang,
                          break_long_words=False, break_on_hyphens=False) or [indent.rstrip()]
    return [ln.replace('\x00', ' ') for ln in lines]


def table(rows, indent):
    cells = [[inline(c) for c in r] for r in rows]
    head, body = cells[0], cells[1:]
    n = len(head)
    widths = [max(len(r[i]) for r in cells if i < len(r)) for i in range(n)]
    out = []
    if len(indent) + sum(widths) + 3 * (n - 1) <= WIDTH:   # fits: aligned columns
        def line(r):
            return indent + '   '.join(r[i].ljust(widths[i]) for i in range(n)).rstrip()
        out.append(line(head))
        out.append(indent + '   '.join('-' * w for w in widths))
        out += [line(r) for r in body]
        return out
    # too wide: the first column as a label, the rest wrapped under it ("Header: value" for 3+ columns)
    for r in body:
        out.append(indent + r[0])
        rest = r[1:]
        if n == 2:
            out += wrap(rest[0], indent + '    ')
        else:
            for h, c in zip(head[1:], rest):
                if c:
                    out += wrap('%s: %s' % (h, c), indent + '    ', indent + '      ')
        out.append('')
    if out and out[-1] == '':
        out.pop()
    return out


def convert(md):
    lines = md.replace('\r\n', '\n').split('\n')
    out, i = [], 0
    while i < len(lines):
        l = lines[i]
        s = l.strip()
        indent = l[:len(l) - len(l.lstrip())]
        if s.startswith('|'):                                    # a table (possibly indented under a list item)
            rows = []
            while i < len(lines) and lines[i].strip().startswith('|'):
                row = lines[i].strip().strip('|').split('|')
                if not all(re.fullmatch(r'\s*:?-{3,}:?\s*', c) for c in row):
                    rows.append([c.strip() for c in row])
                i += 1
            out += table(rows, indent)
            continue
        if s == '---':
            out.append('')
            i += 1
            continue
        m = re.match(r'(#+)\s+(.*)', s)
        if m:
            level, title = len(m.group(1)), inline(m.group(2))
            if level == 1:
                out += ['=' * len(title), title, '=' * len(title)]
            elif level == 2:
                out += [title.upper(), '-' * len(title)]
            else:
                out += [title, '~' * len(title)]
            i += 1
            continue
        m = re.match(r'(\s*)([-*]|\d+\.)\s+(.*)', l)
        if m:                                                    # list item, with its continuation lines
            ind, mark, text = m.group(1), m.group(2), m.group(3)
            bullet = '- ' if mark in '-*' else mark + ' '
            i += 1
            while i < len(lines) and lines[i].strip() and not re.match(r'\s*([-*]|\d+\.)\s', lines[i]) \
                    and not lines[i].strip().startswith('|') and not lines[i].strip().startswith('#'):
                text += ' ' + lines[i].strip()
                i += 1
            out += wrap(inline(text), ind + bullet, ind + ' ' * len(bullet))
            continue
        if not s:
            out.append('')
            i += 1
            continue
        para = [s]                                               # paragraph
        i += 1
        while i < len(lines) and lines[i].strip() and not re.match(r'\s*([-*]|\d+\.|#+|\|)\s?', lines[i]) \
                and lines[i].strip() != '---':
            para.append(lines[i].strip())
            i += 1
        out += wrap(inline(' '.join(para)), indent)
    clean = []                                                   # at most one empty line in a row, none at the ends
    for l in out:
        if l == '' and (not clean or clean[-1] == ''):
            continue
        clean.append(l.rstrip())
    while clean and clean[-1] == '':
        clean.pop()
    return clean


def main(src, dst):
    lines = convert(open(src, encoding='utf-8').read())
    leftovers = [l for l in lines if '**' in l or '`' in l or re.search(r'\]\(', l) or re.match(r'\s*\|', l)]
    if leftovers:
        raise SystemExit('Markdown left in the readme: %s' % leftovers[:3])
    open(dst, 'w', encoding='utf-8-sig', newline='\r\n').write('\n'.join(lines) + '\n')
    print('readme: %d lines, longest %d -> %s' % (len(lines), max(len(l) for l in lines), dst))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
