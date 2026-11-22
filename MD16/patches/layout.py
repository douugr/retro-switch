#!/usr/bin/env python3
"""Gera a tabela que converte um struct do layout do Switch (arm64) para o do iPad (armv7) e de volta.

Entrada: a saída de `clang -Xclang -fdump-record-layouts` para os dois alvos.
Saída (C): runs {nativo, canônico, tamanho} dos campos copiados como bytes, e longs {nativo, canônico}
dos campos `long`/`time_t` (8 bytes no Switch, 4 no iPad). Ponteiros ficam de fora: no formato
canônico eles vão zerados e o núcleo os refaz ao carregar.

Uso: layout.py <dump arm64> <dump armv7> <Tipo> <nome_da_tabela>
"""
import re, sys

SCALAR = {}
for n in ('UINT8 INT8 uint8 int8 uint8_t int8_t char _Bool bool').split(): SCALAR[n] = 1
for n in ('unsigned char', 'signed char'): SCALAR[n] = 1
for n in ('UINT16 INT16 uint16 int16 uint16_t int16_t short').split(): SCALAR[n] = 2
for n in ('unsigned short', 'short int', 'unsigned short int'): SCALAR[n] = 2
for n in ('UINT32 INT32 uint32 int32 uint32_t int32_t int float').split(): SCALAR[n] = 4
for n in ('unsigned int', 'signed int'): SCALAR[n] = 4
for n in ('double int64_t uint64_t').split(): SCALAR[n] = 8
for n in ('long long', 'unsigned long long'): SCALAR[n] = 8
LONG = {'long', 'unsigned long', 'long int', 'unsigned long int', 'time_t', 'size_t', 'ssize_t',
        'intptr_t', 'uintptr_t', 'clock_t', 'off_t'}


class Node:
    def __init__(self, off, typ, name):
        self.off, self.typ, self.name, self.kids = off, typ, name, []


def parse(path):
    """Tipo -> (nó raiz com os filhos aninhados, sizeof)."""
    blocks, stack, root = {}, [], None
    for line in open(path):
        m = re.match(r'^\s*(\d*) \|( *)(.*?)\s*$', line)
        if not m:
            continue
        off, ind, rest = m.groups()
        sm = re.match(r'\[sizeof=(\d+), align=(\d+)', rest.strip())
        if sm:
            if root is not None:
                blocks[root.typ] = (root, int(sm.group(1)))
            root, stack = None, []
            continue
        if not off:
            continue
        depth = (len(ind) - 1) // 2
        if depth == 0:
            name = rest.replace('struct ', '').replace('union ', '')
            root = Node(int(off), name, '')
            root.is_union = rest.startswith('union ')
            stack = [root]
            continue
        if root is None:
            continue
        t, n = rest.rsplit(' ', 1)
        node = Node(int(off), t.replace('const ', '').replace('volatile ', '').strip(), n)
        del stack[depth:]
        stack[-1].kids.append(node)
        stack.append(node)
    return blocks


def is_union(node):
    return len(node.kids) >= 2 and all(k.off == node.off for k in node.kids)


def leaves(blocks, node, base, prefix):
    """Folhas (nome, offset absoluto, tamanho ou None, tipo) dos filhos de node; base = deslocamento de node.off."""
    out = []
    for k in node.kids:
        off = base + (k.off - node.off)
        if k.kids:                                   # struct/union expandido no próprio dump
            if is_union(k) or k.typ.startswith('union'):
                ext = max((e[1] + (e[2] or 8)) for e in leaves(blocks, k, off, '')) - off
                out.append((prefix + k.name, off, ext, 'raw'))
            else:
                out += leaves(blocks, k, off, prefix + k.name + '.')
            continue
        m = re.match(r'^(.*?)((\[\d+\])*)$', k.typ)
        et = m.group(1).strip()
        cnt = 1
        for x in re.findall(r'\[(\d+)\]', m.group(2)):
            cnt *= int(x)
        if '*' in et or '(' in k.typ:
            out.append((prefix + k.name, off, None, 'ptr'))
        elif et in LONG:
            for i in range(cnt):
                out.append((prefix + k.name + '[%d]' % i, off, i, 'long'))
        elif et in SCALAR:
            out.append((prefix + k.name, off, SCALAR[et] * cnt, 'raw'))
        else:
            st = et.replace('struct ', '').replace('union ', '')
            if st not in blocks:
                raise SystemExit('tipo desconhecido: %r (%s)' % (k.typ, prefix + k.name))
            sub, size = blocks[st]
            if is_union(sub) or getattr(sub, 'is_union', False):
                out.append((prefix + k.name, off, size * cnt, 'raw'))
                continue
            for i in range(cnt):
                out += leaves(blocks, sub, off + i * size, prefix + k.name + '[%d].' % i)
    return out


def table(native_path, canon_path, typ):
    a, b = parse(native_path), parse(canon_path)
    fa = leaves(a, a[typ][0], 0, '')
    fb = leaves(b, b[typ][0], 0, '')
    assert [x[0] for x in fa] == [x[0] for x in fb], 'campos diferentes'
    runs, longs = [], []
    for (n, on, sz, k), (_, oc, szc, kc) in zip(fa, fb):
        if k == 'ptr':
            continue
        if k == 'long':
            longs.append((on + 8 * sz, oc + 4 * sz))
            continue
        assert sz == szc, (n, sz, szc)
        if runs and runs[-1][0] + runs[-1][2] == on and runs[-1][1] + runs[-1][2] == oc:
            runs[-1][2] += sz
        else:
            runs.append([on, oc, sz])
    return runs, longs, a[typ][1], b[typ][1]


if __name__ == '__main__':
    nat, can, typ, name = sys.argv[1:5]
    runs, longs, ns, cs = table(nat, can, typ)
    print('/* %s: %d bytes no Switch, %d bytes no formato do iPad (armv7) */' % (typ, ns, cs))
    print('static const rf_run %s[] = {' % name)
    for r in runs:
        print('  {%d, %d, %d},' % tuple(r))
    print('  {0, 0, 0}\n};')
    if longs:
        print('static const rf_long %s_longs[] = {' % name)
        for l in longs:
            print('  {%d, %d},' % l)
        print('};')
    print('#define %s_NLONGS %d' % (name, len(longs)))
    print('#define %s_NATIVE_SZ %d\n#define %s_CANON_SZ %d' % (name, ns, name, cs))
