#!/usr/bin/env python3
"""C register headers for one Adreno generation from the freedreno register
database (MIT; Linux drivers/gpu/drm/msm/registers/adreno/*.xml, fetched by
fetch-adreno-xml.sh). docs/GPU_A618_PLAN.md: no hand-typed GPU offsets.

  gen-adreno-regs.py XMLDIR OUTDIR [--chip 6] [--only REGEX]

Writes OUTDIR/<file>.h for a6xx.xml, a6xx_gmu.xml, adreno_pm4.xml and
adreno_common.xml, keeping only what applies to chip A<chip>XX:
  REG_<P>_<NAME>              dword offset (reg32/reg64)
  REG_<P>_<NAME>(i0)          array element, or a register inside one
  <P>_<NAME>_<FIELD>__MASK    bitfields (inline or through type="bitset")
  <P>_<NAME>_<FIELD>__SHIFT
  <VALUE>                     enum values, with the enum named in a comment
<P> is the chip (A6XX) in prefix="variant"/"chip" domains, else the domain
name. Offsets are in dwords. Both A6XX domains (a6xx.xml and a6xx_gmu.xml)
count from the GPU at 0x5000000, as kgsl does (GMU_CM3_ITCM_START 0x1b400 =
0x506d000), except the RSCC_* / GPU_RSCC_* registers, which count from the
RSCC block (GMU 0x506a000 + 0x23000 on A618), and A6XX_PDC / A6XX_PDC_GPU_SEQ,
which count from their PDC blocks. Packet-layout domains (CP_*) are skipped.
--only keeps register and field macros whose name matches REGEX (enums stay).
"""
import argparse, os, re, sys
import xml.etree.ElementTree as ET

FILES = ['a6xx.xml', 'a6xx_gmu.xml', 'adreno_pm4.xml', 'adreno_common.xml']
SKIP_DOMAIN = re.compile(r'^(CP_|ir3_)')	# packet field layouts and shader stats, not registers
CHIP = re.compile(r'^A(\d)XX(?:-(?:A(\d)XX)?)?$')
CHIP_DOMAIN = re.compile(r'^A(\d)XX_')


def tag(e):
    return e.tag.rsplit('}', 1)[-1]


def applies(variants, chip):
    """True if a variants="A5XX-A6XX,..." list covers chip; non-chip varsets always do."""
    if not variants:
        return True
    for tok in variants.split(','):
        m = CHIP.match(tok.strip())
        if not m:
            return True
        lo = int(m.group(1))
        hi = int(m.group(2)) if m.group(2) else (99 if tok.strip().endswith('-') else lo)
        if lo <= chip <= hi:
            return True
    return False


def num(s):
    return int(s, 0)


class Gen:
    def __init__(self, chip, only):
        self.chip, self.only = chip, only
        self.bitsets = {}
        self.lines, self.seen, self.conflicts = [], {}, 0

    def emit(self, name, value, comment=''):
        if self.only and not self.only.search(name):
            return
        if name in self.seen:
            if self.seen[name] != value:
                print(f'conflict: {name} = {self.seen[name]} and {value}; keeping the first', file=sys.stderr)
                self.conflicts += 1
            return
        self.seen[name] = value
        self.lines.append(f'#define {name:<56} {value}{comment}')

    def fields(self, prefix, reg):
        """Bitfields of reg: its own children, then those of the bitset its type= names."""
        out = [f for f in reg if tag(f) == 'bitfield']
        t = reg.get('type')
        if t in self.bitsets:
            out += [f for f in self.bitsets[t] if tag(f) == 'bitfield']
        for f in out:
            if not applies(f.get('variants'), self.chip):
                continue
            if f.get('pos') is not None:
                lo = hi = num(f.get('pos'))
            else:
                lo, hi = num(f.get('low')), num(f.get('high'))
            mask = ((1 << (hi - lo + 1)) - 1) << lo
            self.emit(f'{prefix}_{f.get("name")}__MASK', f'0x{mask:08x}u')
            self.emit(f'{prefix}_{f.get("name")}__SHIFT', f'{lo}')

    def regs(self, node, p, base, idx):
        """Registers under node; base is a C expression for the containing offset."""
        for e in node:
            t = tag(e)
            if t in ('reg32', 'reg64', 'array', 'stripe') and not applies(e.get('variants'), self.chip):
                continue
            if t == 'stripe':
                self.regs(e, p, base, idx)
            elif t in ('reg32', 'reg64'):
                off = num(e.get('offset'))
                expr = f'0x{off:05x}u' if base is None else f'({base} + 0x{off:x}u)'
                args = '(' + ', '.join(idx) + ')' if idx else ''
                width = '' if t == 'reg32' else '\t/* 64-bit: lo, hi = +1 */'
                self.emit(f'REG_{p}_{e.get("name")}{args}', expr, width)
                self.fields(f'{p}_{e.get("name")}', e)
            elif t == 'array':
                off, stride = num(e.get('offset')), num(e.get('stride'))
                i = f'i{len(idx)}'
                start = f'0x{off:05x}u' if base is None else f'{base} + 0x{off:x}u'
                expr = f'({start} + 0x{stride:x}u * ({i}))'
                kids = [k for k in e if tag(k) in ('reg32', 'reg64', 'array', 'stripe')]
                if not kids:
                    self.emit(f'REG_{p}_{e.get("name")}({", ".join(idx + [i])})', expr,
                              f'\t/* [{e.get("length")}] */')
                else:
                    self.regs(e, f'{p}_{e.get("name")}', expr, idx + [i])

    def enums(self, root):
        for en in root.iter():
            if tag(en) != 'enum' or not applies(en.get('variants'), self.chip):
                continue
            nxt = 0
            vals = []
            for val in en:
                if tag(val) != 'value':
                    continue
                n = num(val.get('value')) if val.get('value') is not None else nxt
                nxt = n + 1
                if applies(val.get('variants'), self.chip):
                    vals.append((val.get('name'), n))
            if vals:
                self.lines.append(f'\n/* enum {en.get("name")} */')
                for name, n in vals:
                    if name not in self.seen:
                        self.seen[name] = hex(n)
                        self.lines.append(f'#define {name:<56} 0x{n:x}')

    def domains(self, root):
        for d in root.iter():
            if tag(d) != 'domain' or SKIP_DOMAIN.match(d.get('name')):
                continue
            m = CHIP_DOMAIN.match(d.get('name'))
            if not applies(d.get('variants'), self.chip) or (m and int(m.group(1)) != self.chip):
                continue
            p = f'A{self.chip}XX' if d.get('prefix') in ('variant', 'chip') else d.get('name')
            self.lines.append(f'\n/* domain {d.get("name")} */')
            self.regs(d, p, None, [])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('xmldir'); ap.add_argument('outdir')
    ap.add_argument('--chip', type=int, default=6)
    ap.add_argument('--only', type=re.compile)
    a = ap.parse_args()
    trees = {f: ET.parse(os.path.join(a.xmldir, f)).getroot()
             for f in os.listdir(a.xmldir) if f.endswith('.xml')}
    cr = trees['freedreno_copyright.xml']
    lic = cr.find('.//{*}license').text.strip()
    year = cr.find('.//{*}copyright').get('year')
    authors = ', '.join(e.get('name') for e in cr.iter() if tag(e) == 'author')
    source = open(os.path.join(a.xmldir, 'SOURCE.txt')).readline().strip() \
        if os.path.exists(os.path.join(a.xmldir, 'SOURCE.txt')) else 'unknown'
    bitsets = {}
    for root in trees.values():
        for b in root.iter():
            if tag(b) == 'bitset':
                bitsets.setdefault(b.get('name'), b)
    os.makedirs(a.outdir, exist_ok=True)
    total = 0
    for f in FILES:
        g = Gen(a.chip, a.only)
        g.bitsets = bitsets
        g.domains(trees[f])
        g.enums(trees[f])
        guard = re.sub(r'\W', '_', f).upper() + '_H'
        out = os.path.join(a.outdir, f + '.h')
        with open(out, 'w', newline='\n') as o:
            o.write(f'/*\n * Generated by tools/sunfish/gen-adreno-regs.py --chip {a.chip}'
                    f'{" --only " + a.only.pattern if a.only else ""}; do not edit.\n'
                    f' * Source: {f} from {source}\n *\n'
                    f' * freedreno register database. Copyright {year} {authors} and contributors.\n *\n')
            o.write(''.join(f' * {l}\n'.replace(' * \n', ' *\n') for l in lic.splitlines()))
            o.write(f' */\n#ifndef {guard}\n#define {guard}\n')
            o.write('\n'.join(g.lines))
            o.write(f'\n\n#endif /* {guard} */\n')
        total += g.conflicts
        print(f'{out}: {len(g.seen)} definitions, {g.conflicts} conflicts')
    sys.exit(1 if total else 0)


if __name__ == '__main__':
    main()
