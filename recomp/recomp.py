"""FFXiMain recompiler driver.

  python recomp/recomp.py --meta <meta.json> --image <unpacked.dll> --out generated/ \
      [--functions 0x10317480,0x10312980,...] [--all] [--stats] [--hooks name=0x100863d6,...]
      [--wraps name=0x10080dc0,...] [--patches <patches.json>]

--functions translates those entries plus everything they reach by direct call or tail jump
(the closure), which is what a differential test needs. --all translates every function in the
metadata. Output: funcs.h (prototypes), funcs_NNN.c (translations), table.c (address -> function
table for indirect calls), and with --stats a coverage report of unimplemented instructions.

--hooks names instructions where the host may step in (meta/builds.json "hooks"): before each, the
translation calls the host function pointer rt_hook_<name> (GuestFn, defined in table.c, NULL until
the host sets it) with the guest's registers stored, and reloads them after.

--wraps names whole functions the host may replace (meta/builds.json "wraps"): the translation of
f_XXXXXXXX becomes f_XXXXXXXX_body, and f_XXXXXXXX calls the host's rt_wrap_<name> (NULL until the
host sets it) instead of it when set. The host runs at the function's entry (esp at the return
address) and either calls rt_orig_<name> (the body) or returns for it (runtime.h rt_return).
Every call reaches the wrap: direct calls, tail jumps and rt_table (indirect calls, vtables).

--patches names code patches addons make ({"group": {"0xADDR": "hexbytes", ...}, ...}, from
meta/builds.json "patches"): the bytes an addon writes over the game's code, which the recompiled C
would otherwise never run. Each function a group touches is translated a second time with the
group's bytes in place (f_XXXXXXXX_p_<group>), and runs instead of the original while the host sets
rt_patch_<group> (host/addons/patch.c: while those bytes are in guest memory). table.c lists the
patches (rt_patches) for the host to check.
"""
import argparse
import collections
import json
import os
import re
import sys

import capstone
import pefile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86c import FunctionTranslator  # noqa: E402


# Instructions a 32-bit C/C++ compiler never emits: a run of bytes that decodes to one of them is a
# table or padding, not code (ffxivgame.exe keeps pointer tables in .text's gaps: 0x0057ce35
# repeated decodes as `into`).
NOT_COMPILED = {
    'in', 'out', 'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd', 'hlt', 'cli', 'sti', 'into', 'int1',
    'aaa', 'aad', 'aam', 'aas', 'daa', 'das', 'salc', 'les', 'lds', 'lss', 'retf', 'iret', 'iretd', 'bound',
    'arpl', 'ljmp', 'lcall', 'icebp', 'sldt', 'sgdt', 'sidt', 'lgdt', 'lidt', 'clts',
}


class Program:
    def __init__(self, meta, image_path):
        self.meta = meta
        self.prefix = 'f_'  # translated function names: f_XXXXXXXX (FFXiMain), <module>_XXXXXXXX otherwise
        pe = pefile.PE(image_path, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        if self.base != meta['image_base']:
            raise SystemExit('image base %#x does not match metadata %#x' % (self.base, meta['image_base']))
        self.image = pe.get_memory_mapped_image()
        for va, data in meta.get('apply', {}).items():
            # code patches made part of the image before anything is decoded (--apply)
            img = bytearray(self.image)
            img[va - self.base:va - self.base + len(data)] = data
            self.image = bytes(img)
        # IAT slot -> imported name: the translator inlines the hottest imports at their calls
        self.imports = {}
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
        for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            for imp in d.imports:
                if imp.name:
                    self.imports[imp.address] = imp.name.decode()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.functions = {f['entry']: f['ranges'] for f in meta['functions']}
        self.meta_entries = set(self.functions)
        self.entries = set(self.functions)
        self.switches = {s['at']: s['targets'] for s in meta['switches'] if s['targets']}
        self.referenced = set()
        self.hooks = {}  # address -> name (--hooks)
        self.hooked = set()
        self.wraps = {}  # function entry -> name (--wraps)
        self.patches = {}  # group -> {address: bytes} (--patches)
        self.overlay = None  # while translating a patched variant: {address: byte}
        self.relocs = self.text_relocations(pe)
        self.inner = self.add_data_entries(pe)
        self.inner |= self.add_branch_entries()
        if not self.has_reloc:
            # No relocation table: the code pointers in each newly found function's operands can
            # lead to more functions (an initializer reached through the .CRT$XC table pushes
            # its destructor stub for atexit: 0xf3c2f0). Repeat until nothing new turns up.
            scanned = set(self.functions_scanned)
            while True:
                new = [e for e in self.functions if e not in scanned]
                if not new:
                    break
                scanned |= set(new)
                self.relocs |= self.operand_addresses(pe, new)
                spans = sorted((lo, hi, entry) for entry, ranges in self.functions.items() for lo, hi in ranges)
                self.add_code_pointer_entries(spans, [lo for lo, _, _ in spans])
                self.inner |= self.add_branch_entries()

    def add_data_entries(self, pe):
        """Code the image's data points at (vtables, callbacks, exception handlers) is called
        indirectly, so each such address must be an entry. The metadata misses some: Ghidra folds a
        small function into the one that tail-jumps to it (FFXiMain 2026-09-03: 0x100542e0, slot 0
        of the vtable at 0x1032b69c, a range of 0x10054460), and leaves a few out altogether
        (0x1019d090). A folded one becomes an entry with its host's ranges (the translation starts
        at the entry, and every branch back into the host stays inside it); one outside every
        range, when it decodes as code up to a ret. Returns the entries that sit inside another
        function's body (the Windows loader must not put a 5-byte jmp there)."""
        import bisect
        import struct
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
        text_lo, text_hi = self.meta['text']
        data_sites = self.data_relocations(pe)
        spans = sorted((lo, hi, entry) for entry, ranges in self.functions.items() for lo, hi in ranges)
        starts = [lo for lo, _, _ in spans]
        inner = set()
        for rva in data_sites:
            if True:
                target = struct.unpack_from('<I', self.image, rva)[0]
                if not text_lo <= target < text_hi or target in self.functions:
                    continue
                i = bisect.bisect_right(starts, target) - 1
                if i >= 0 and spans[i][0] <= target < spans[i][1]:
                    if not self.on_boundary(spans[i][2], target):
                        continue  # inside one of the host's instructions: a table in .text, not code
                    self.functions[target] = [list(r) for r in self.functions[spans[i][2]]]
                    inner.add(target)
                else:
                    end = self.code_until_ret(target)
                    if end:
                        self.functions[target] = [[target, end]]
        if not self.has_reloc:
            # Unaligned pointers in data: the CRT's x87 dispatch tables pack code addresses at odd
            # offsets (ffxivgame.exe 0x12ebeaa holds 0x9ee8b1). Only code outside every known body
            # that decodes cleanly to a ret is taken; inside a body they are left alone (a random
            # unaligned dword would otherwise become an inner entry, a whole function's copy).
            for target in self.unaligned_data_pointers(pe):
                i = bisect.bisect_right(starts, target) - 1
                if target in self.functions or (i >= 0 and spans[i][0] <= target < spans[i][1]):
                    continue
                end = self.code_until_ret(target)
                if end and end - target >= 4:
                    self.functions[target] = [[target, end]]
        self.add_code_pointer_entries(spans, starts)
        self.entries = set(self.functions)
        return inner

    def unaligned_data_pointers(self, pe):
        """Code addresses at unaligned offsets of the data sections."""
        import struct
        text_lo, text_hi = self.meta['text']
        out = set()
        for sec in pe.sections:
            if sec.Name.rstrip(b'\0') not in (b'.data', b'.rdata'):
                continue
            lo = sec.VirtualAddress
            data = self.image[lo:lo + min(sec.Misc_VirtualSize, sec.SizeOfRawData)]
            for k in (1, 2, 3):
                for v in struct.unpack_from('<%dI' % ((len(data) - k) // 4), data, k):
                    if text_lo <= v < text_hi:
                        out.add(v)
        return sorted(out)

    def add_code_pointer_entries(self, spans, starts):
        """The same for code that takes a function's address as an immediate - a callback stored
        into a structure, an exception handler pushed - when the function lies outside every range
        (FFXiMain 2026-09-03: 0x100a30b4 stores 0x100a3130, called through [esi+0x54] when the
        settings menu opens). Those addresses are in .text's own relocations. Only immediates
        count: an address used as a displacement is a table read (switch tables, byte maps), and
        tables decode as code."""
        import bisect
        import struct
        text_lo, text_hi = self.meta['text']
        tables = {s['table'] for s in self.meta['switches']}
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = True

        def span_of(a):
            i = bisect.bisect_right(starts, a) - 1
            return spans[i] if i >= 0 and spans[i][0] <= a < spans[i][1] else None

        decoded = {}
        for site in sorted(self.relocs):
            host = span_of(site)
            if not host:
                continue  # a table in a gap, not an instruction
            target = struct.unpack_from('<I', self.image, site - self.base)[0]
            if not text_lo <= target < text_hi or target in self.functions or target in tables:
                continue
            inside = span_of(target)
            if inside and not self.has_reloc_or_ghidra(inside[2]) and self.on_boundary(inside[2], target):
                # inside a range discovery guessed (code_until_ret), on one of its instruction
                # boundaries: an entry of its own, as add_data_entries does
                self.functions[target] = [list(r) for r in self.functions[inside[2]]]
                continue
            if inside:
                continue
            entry = host[2]
            if entry not in decoded:
                ins = sorted((i for lo, hi in self.functions[entry] for i in md.disasm(self.read(lo, hi - lo), lo)),
                             key=lambda i: i.address)
                decoded[entry] = ([i.address for i in ins], ins)
            addrs, ins = decoded[entry]
            k = bisect.bisect_right(addrs, site) - 1
            if k < 0:
                continue
            i = ins[k]
            if not i.imm_offset or i.address + i.imm_offset != site:
                continue  # a displacement: data
            end = self.code_until_ret(target)
            if end:
                self.functions[target] = [[target, end]]

    def infer_switch(self, ins):
        """Case targets of a `jmp [reg*4 + table]` the metadata does not list (code Ghidra never
        made a function: the CRT's printf core at 0x9e5e11 in ffxivgame.exe), or None.

        The count comes from the MSVC guard when one decodes in the instructions just before the
        jump (cmp reg, N ; ja default ; [movzx reg, byte [reg + index_table]]), as
        discovery/Switches.py reads it; failing that, entries are read while each is a code
        address near the jump. Cached in self.switches."""
        if ins.address in self.switches:
            return self.switches[ins.address]
        from capstone import x86_const as X
        if len(ins.operands) != 1 or ins.operands[0].type != X.X86_OP_MEM:
            return None
        m = ins.operands[0].mem
        text_lo, text_hi = self.meta['text']
        if m.base or not m.index or m.scale != 4 or not text_lo <= (m.disp & 0xFFFFFFFF) < self.base + len(self.image):
            return None
        table = m.disp & 0xFFFFFFFF
        index = ins.reg_name(m.index)
        count, idx_table = None, None
        # the guard: the longest run of instructions that decodes up to the jump exactly
        for back in range(48, 1, -1):
            seq = list(self.md.disasm(self.read(ins.address - back, back), ins.address - back))
            if not seq or seq[-1].address + seq[-1].size != ins.address:
                continue
            for p in reversed(seq[-10:]):
                if p.mnemonic in ('movzx', 'mov') and idx_table is None and len(p.operands) == 2 \
                        and p.operands[1].type == X.X86_OP_MEM and p.operands[1].size == 1 \
                        and (p.operands[1].mem.index or p.operands[1].mem.base) and p.operands[1].mem.disp:
                    idx_table = p.operands[1].mem.disp & 0xFFFFFFFF
                if p.mnemonic == 'cmp' and len(p.operands) == 2 and p.operands[1].type == X.X86_OP_IMM:
                    n = p.operands[1].imm & 0xFFFFFFFF
                    if n < 4096:
                        count = n + 1
                    break
            break
        if idx_table is not None and not 0 <= idx_table - self.base <= len(self.image) - (count or 0):
            idx_table = None  # a byte load off the stack, not an index table (ffxivupdater.exe)
        if count is not None and idx_table is not None:
            count = max(self.image[idx_table - self.base + i] for i in range(count)) + 1
        targets = []
        limit = count if count is not None else 1024
        for i in range(limit):
            off = table - self.base + 4 * i
            if off + 4 > len(self.image):
                break
            v = int.from_bytes(self.image[off:off + 4], 'little')
            if not text_lo <= v < text_hi or abs(v - ins.address) > 0x10000:
                if count is not None:
                    return None  # the guard was not this switch's
                break
            targets.append(v)
        if not targets:
            return None
        targets = sorted(set(targets))
        self.switches[ins.address] = targets
        return targets

    def has_reloc_or_ghidra(self, entry):
        """Whether the function at entry came from the metadata (Ghidra), not discovery."""
        return entry in self.meta_entries

    def add_branch_entries(self):
        """The same for direct branches: a call to an address that is not an entry, or a jump
        into another function's body (FFXiMain 2026-09-03: 0x101f320f calls 0x101f0260, a range
        of 0x1007ad70). Each target on one of the host's instruction boundaries becomes an entry,
        so the branch is a call or tail call into its own translation; one inside an instruction
        comes from data decoded as code (0x10069500 "jumps" into the operand of a call) and is left. Returns the
        entries inside another function's body."""
        import bisect
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)  # no detail: fast
        text_lo, text_hi = self.meta['text']
        spans = sorted((lo, hi, entry) for entry, ranges in self.functions.items() for lo, hi in ranges)
        starts = [lo for lo, _, _ in spans]

        def span_of(a):
            i = bisect.bisect_right(starts, a) - 1
            return spans[i] if i >= 0 and spans[i][0] <= a < spans[i][1] else None

        targets = {}
        for entry, ranges in list(self.functions.items()):
            own = [(lo, hi) for lo, hi in ranges]
            for lo, hi in ranges:
                for addr, size, mn, op in md.disasm_lite(self.read(lo, hi - lo), lo):
                    if mn != 'call' and not mn.startswith('j') and mn not in ('loop', 'loope', 'loopne'):
                        continue
                    if not op.startswith('0x'):
                        continue
                    t = int(op, 16)
                    if t in self.functions or not text_lo <= t < text_hi:
                        continue
                    if mn == 'call' or not any(a <= t < b for a, b in own):
                        targets.setdefault(t, mn == 'call')
        inner = set()
        for t, is_call in sorted(targets.items()):
            s = span_of(t)
            if s and s[2] != t and not self.on_boundary(s[2], t):
                continue  # inside one of the host's instructions: the branch is data decoded as code
            if s and s[2] != t:
                self.functions[t] = [list(r) for r in self.functions[s[2]]]
                inner.add(t)
            elif not s and is_call:
                end = self.code_until_ret(t)
                if end:
                    self.functions[t] = [[t, end]]
        self.entries = set(self.functions)
        return inner

    def on_boundary(self, host, a):
        """Whether a starts an instruction of host's ranges, decoded linearly."""
        if not hasattr(self, '_boundaries'):
            self._boundaries = {}
            self._md_lite = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        if host not in self._boundaries:
            self._boundaries[host] = {i[0] for lo, hi in self.functions[host]
                                      for i in self._md_lite.disasm_lite(self.read(lo, hi - lo), lo)}
        return a in self._boundaries[host]

    def code_until_ret(self, va, limit=0x1000):
        """The end of straight-line code from va to its first ret, or 0 if it does not look like
        code (it runs out, or reads ports: data tables inside .text decode as in/ins)."""
        for ins in self.md.disasm(self.read(va, limit), va):
            if ins.mnemonic in NOT_COMPILED or ins.mnemonic.split(' ')[-1] in NOT_COMPILED:
                return 0
            if ins.mnemonic in ('push', 'pop') and ins.op_str in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
                return 0
            if ins.mnemonic.startswith('ret') or ins.mnemonic == 'jmp':
                # the end of straight-line code: the translation follows jumps itself (x86c.py
                # decode), so a range must not run on through padding into the next function
                return ins.address + ins.size
            if ins.mnemonic == 'int3':
                return ins.address  # padding: the code ended in a call that does not return
        return 0

    def data_relocations(self, pe):
        """The RVAs outside .text that hold an absolute address of code: the image's own
        relocations when it has them (FFXiMain); else, for an EXE linked at a fixed base without a
        relocation table (ffxivgame.exe), every aligned dword in the data sections whose value lies
        in .text. A false hit is harmless here: it only becomes an entry when it starts an
        instruction (on_boundary) or decodes as code up to a ret."""
        import struct
        blocks = getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', None)
        if blocks:
            return [e.rva for b in blocks for e in b.entries if e.type == 3]  # IMAGE_REL_BASED_HIGHLOW
        text_lo, text_hi = self.meta['text']
        out = []
        for sec in pe.sections:
            if sec.Name.rstrip(b'\0') in (b'.text', b'.rsrc', b'.reloc'):
                continue
            lo = sec.VirtualAddress
            hi = lo + min(sec.Misc_VirtualSize, sec.SizeOfRawData)
            for rva in range(lo, hi - 3, 4):
                if text_lo <= struct.unpack_from('<I', self.image, rva)[0] < text_hi:
                    out.append(rva)
        return out

    def text_relocations(self, pe):
        """Every location in .text that holds an absolute image address.

        An EXE without a .reloc section (ffxivgame.exe) always loads at its preferred base, so
        nothing needs relocating (RD is 0); its code pointers are found from the instructions
        themselves: every immediate and displacement whose value lies inside the image.

        They are not in the PE relocation directory (which covers .rdata/.data only): the packer's
        stub applies them itself from a private table at the start of .reloc, in the ordinary
        base-relocation block format, ending at the first block past .text or of size 0.
        """
        import struct
        self.has_reloc = any(s.Name.rstrip(b'\0') == b'.reloc' for s in pe.sections)
        if not self.has_reloc:
            self.functions_scanned = set(self.functions)
            return self.operand_addresses(pe, list(self.functions))
        reloc = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.reloc')
        text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
        text_end = text.VirtualAddress + text.Misc_VirtualSize
        off = reloc.VirtualAddress
        out = set()
        while True:
            page, size = struct.unpack_from('<II', self.image, off)
            if size == 0 or page >= text_end:
                break
            for k in range((size - 8) // 2):
                e = struct.unpack_from('<H', self.image, off + 8 + 2 * k)[0]
                if e >> 12 == 3:  # IMAGE_REL_BASED_HIGHLOW
                    out.add(self.base + page + (e & 0xFFF))
            off += size
        return out

    def operand_addresses(self, pe, entries):
        """Sites in .text (immediate or displacement fields) holding an address inside the image:
        what a relocation table would list, read from the given functions' code."""
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = True
        lo_img, hi_img = self.base, self.base + pe.OPTIONAL_HEADER.SizeOfImage
        out = set()
        for entry in entries:
            for lo, hi in self.functions[entry]:
                for ins in md.disasm(self.read(lo, hi - lo), lo):
                    for off, size in ((ins.imm_offset, ins.imm_size), (ins.disp_offset, ins.disp_size)):
                        if off and size == 4:
                            v = int.from_bytes(ins.bytes[off:off + 4], 'little')
                            if lo_img <= v < hi_img:
                                out.add(ins.address + off)
        return out

    def read(self, va, n):
        off = va - self.base
        b = self.image[off:off + n]
        if self.overlay:
            b = bytearray(b)
            for a, v in self.overlay.items():
                if va <= a < va + n:
                    b[a - va] = v
            b = bytes(b)
        return b

    def function_of(self, a):
        """The entry of the function whose ranges hold address a (None if none does)."""
        for e, ranges in self.functions.items():
            for lo, hi in ranges:
                if lo <= a < hi:
                    return e
        return None


def write_if_changed(path, text):
    """Leave unchanged files alone so an incremental C build only recompiles what moved."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, 'w') as f:
        f.write(text)


def patch_kinds(prog, entries):
    """0 if a 5-byte jmp fits at the entry without touching another entry or live bytes, else 1."""
    all_entries = sorted(prog.entries)
    nxt = {a: b for a, b in zip(all_entries, all_entries[1:])}
    kinds = []
    for e in entries:
        ok = e not in prog.inner and nxt.get(e, e + 5) - e >= 5
        if ok:
            end = max(hi for lo, hi in prog.functions[e] if lo <= e < hi) if any(lo <= e < hi for lo, hi in prog.functions[e]) else e
            if end < e + 5:
                tail = prog.read(end, e + 5 - end)
                ok = all(b in (0xCC, 0x90) for b in tail)
        kinds.append(0 if ok else 1)
    return kinds


def retail_dll():
    import winreg
    for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
        try:
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\PlayOnlineUS\InstallFolder', 0, winreg.KEY_READ | view)
            return os.path.join(winreg.QueryValueEx(k, '0001')[0], 'FFXiMain.dll')
        except OSError:
            continue
    raise SystemExit('FINAL FANTASY XI install not found in the registry; pass --retail')


def image_constants(prog, image_path):
    """What the loader needs to map the retail EXE and check it is the right build."""
    pe = pefile.PE(image_path, fast_load=True)
    text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
    if any(s.Name.rstrip(b'\0') == b'.reloc' for s in pe.sections):
        raise SystemExit('%s: relocatable images are not supported; the game\'s EXEs load at a fixed base' % image_path)
    return {
        'base': prog.base, 'reloc_rva': 0, 'timestamp': pe.FILE_HEADER.TimeDateStamp,
        'size': pe.OPTIONAL_HEADER.SizeOfImage, 'text_rva': text.VirtualAddress,
        'text_size': text.Misc_VirtualSize, 'packed_rva': 0, 'packed_len': 0,
        'oep': prog.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint,
    }


REG_NAMES = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
REG_ALL = 0xFF
REG_ESP = 16
ABI_CLOBBER = 1 | 2 | 4 | REG_ESP  # what any callee may change (MSVC: eax, ecx, edx)
ABI_READ = 1 | 2 | 4 | REG_ESP     # what an unknown callee may take


def regs_store(mask, flags):
    """A call's or a return's stores of the registers in mask (esp and the direction flag always)."""
    mask |= REG_ESP
    s = ' '.join('g->%s = %s;' % (n, n) for b, n in enumerate(REG_NAMES) if mask >> b & 1)
    s += ' g->df = (uint8_t)df;'
    if flags:
        s += ' g->cf = (uint8_t)cf; g->pf = (uint8_t)pf; g->af = (uint8_t)af; g->zf = (uint8_t)zf; g->sf = (uint8_t)sf; g->of = (uint8_t)of;'
    return s


def regs_load(mask):
    mask |= REG_ESP
    return (' '.join('%s = g->%s;' % (n, n) for b, n in enumerate(REG_NAMES) if mask >> b & 1) +
            ' cf = g->cf; pf = g->pf; af = g->af; zf = g->zf; sf = g->sf; of = g->of; df = g->df;')


def register_liveness(reginfo):
    """Registers across calls (x86c.py reg_analysis): for each function, the registers it may
    return changed (mod) and the ones it reads on entry (live-in), through its callees. A direct
    call then stores only the callee's live-in registers and reloads only the ones it changes, and
    a return stores only what changed: a caller keeps the rest in its own locals. Calls through
    pointers still store and reload everything, so a function they reach finds its registers in g
    (and what it does not change stays there). Functions the host wraps or patches, hooks, and
    anything not understood keep everything."""
    callers = collections.defaultdict(set)
    for e, ri in reginfo.items():
        for r, w, kind, tgt, succ in ri['insns']:
            if kind in (1, 4):
                callers[tgt].add(e)
    mod = {e: (REG_ALL if ri['unsafe'] else ri['own'] | REG_ESP) for e, ri in reginfo.items()}
    work = set(reginfo)
    while work:
        e = work.pop()
        ri = reginfo[e]
        if ri['unsafe']:
            continue
        m, keep = mod[e], ri['preserved']
        for r, w, kind, tgt, succ in ri['insns']:
            if kind == 1:
                m |= mod.get(tgt, REG_ALL) & ~keep
            elif kind == 2:
                m |= ABI_CLOBBER & ~keep
            elif kind == 4:
                m |= mod.get(tgt, REG_ALL)
            elif kind == 5:
                m |= ABI_CLOBBER
        if m != mod[e]:
            mod[e] = m
            work |= callers[e]
    live_in = {e: (REG_ALL if ri['unsafe'] else 0) for e, ri in reginfo.items()}
    work = set(reginfo)
    while work:
        e = work.pop()
        ri = reginfo[e]
        if ri['unsafe']:
            continue
        ins = ri['insns']
        if not ins:
            v = REG_ALL
        else:
            live = [0] * len(ins)
            changed = True
            while changed:
                changed = False
                for k in range(len(ins) - 1, -1, -1):
                    r, w, kind, tgt, succ = ins[k]
                    after = 0
                    for x in succ:
                        after |= live[x]
                    if kind == 1:
                        v = (after & ~(mod.get(tgt, REG_ALL) | w)) | live_in.get(tgt, REG_ALL) | r
                    elif kind == 2:
                        v = (after & ~(ABI_CLOBBER | w)) | ABI_READ | r
                    elif kind == 3:
                        v = ((mod[e] | REG_ESP) & ~w) | r
                    elif kind == 4:
                        v = (after & ~w) | live_in.get(tgt, REG_ALL) | mod[e] | REG_ESP | r
                    elif kind == 5:
                        v = (after & ~w) | ABI_READ | mod[e] | r
                    else:
                        v = (after & ~w) | r
                    if v != live[k]:
                        live[k] = v
                        changed = True
            v = live[ri['entry']]
        if v != live_in[e]:
            live_in[e] = v
            work |= callers[e]
    # What a return stores: mod, and a register the function saves and restores whose slot in g a
    # call inside it may have overwritten (a call stores its callee's live-in registers from this
    # function's locals, the callee's return stores its own; full stores write everything), since
    # its callers keep the value they had and their own returns rely on g holding it.
    ret = dict(mod)
    work = set(reginfo)
    while work:
        e = work.pop()
        ri = reginfo[e]
        if ri['unsafe'] or not ri['preserved']:
            continue
        dirty = 0
        for r, w, kind, tgt, succ in ri['insns']:
            if kind == 1:
                dirty |= live_in.get(tgt, REG_ALL) | ret.get(tgt, REG_ALL)
            elif kind:
                dirty |= REG_ALL
        v = mod[e] | (ri['preserved'] & dirty)
        if v != ret[e]:
            ret[e] = v
            work |= callers[e]
    n = len(reginfo)
    print('registers across calls: %d functions, %.2f registers changed and %.2f read on entry on average'
          % (n, sum(bin(m).count('1') for m in mod.values()) / max(n, 1),
             sum(bin(v).count('1') for v in live_in.values()) / max(n, 1)), file=sys.stderr)
    return mod, live_in, ret



def gate(prog, e, lines, variants):
    """--wraps and --patches: the translation renamed to <fn>_body; each patched variant as
    <fn>_p_<group>; <fn>_sel picks a variant whose patch is in place, else the body; and the entry
    <fn> itself defers to the host's wrap when there is one, else to the selection."""
    fn = '%s%08x' % (prog.prefix, e)
    head = 'void %s(Guest* RT_RESTRICT g)' % fn
    if lines[0] != head:
        raise SystemExit('--wraps/--patches: unexpected translation header for %#x' % e)
    out = ['void %s_body(Guest* RT_RESTRICT g)' % fn] + lines[1:]
    inner = fn + '_body'
    if variants:
        for group, v in variants:
            if v[0] != head:
                raise SystemExit('--patches: unexpected translation header for %#x' % e)
            out += ['', '/* recomp.py --patches %s: this function with the patch in place */' % group,
                    'void %s_p_%s(Guest* RT_RESTRICT g)' % (fn, group)] + v[1:]
        out += ['', 'void %s_sel(Guest* g)' % fn, '{']
        for group, _ in variants:
            out.append('    if (rt_patch_%s) { %s_p_%s(g); return; }' % (group, fn, group))
        out += ['    %s_body(g);' % fn, '}']
        inner = fn + '_sel'
    out.append('')
    if e in prog.wraps:
        name = prog.wraps[e]
        out += ['/* recomp.py --wraps %s: the host may replace this function */' % name, head, '{',
                '    if (rt_wrap_%s) rt_wrap_%s(g);' % (name, name), '    else %s(g);' % inner, '}']
    else:
        out += [head, '{', '    %s(g);' % inner, '}']
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--meta', required=True)
    ap.add_argument('--image', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--functions', default='')
    ap.add_argument('--entries', default='',
                    help='0xADDR,...: functions the metadata and discovery miss (reached only through '
                         'pointers made at run time); each runs from its address to its first ret')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--chunk', type=int, default=400)
    ap.add_argument('--retail', help='the retail (packed) DLL; default: FFXiMain.dll from the registry')
    ap.add_argument('--hooks', default='', help='name=0xADDR,...: host hook points (see above)')
    ap.add_argument('--wraps', default='', help='name=0xADDR,...: functions the host may replace (see above)')
    ap.add_argument('--apply', default='', help='a JSON file of code patches always translated in (see main)')
    ap.add_argument('--patches', default='', help='a JSON file of addon code patches (see above)')
    ap.add_argument('--module', default='',
                    help='a second module (e.g. ffxi for FFXi.dll): functions are named <module>_XXXXXXXX, the '
                         'module has its own relocation delta, and table.c defines RtModule rt_module_<module> '
                         'instead of the rt_table/rt_image_* globals FFXiMain uses')
    args = ap.parse_args()

    meta = json.load(open(args.meta))
    if args.apply:
        # {"group": {"0xVA": "hex bytes", ...}, ...}; keys starting with "_" are comments
        meta['apply'] = {}
        for group, spots in json.load(open(args.apply)).items():
            if group.startswith('_'):
                continue
            for addr, hexbytes in spots.items():
                if not addr.startswith('_'):
                    meta['apply'][int(addr, 16)] = bytes.fromhex(hexbytes)
    prog = Program(meta, args.image)
    for e in filter(None, args.entries.split(',')):
        a = int(e, 16)
        if a not in prog.functions:
            end = prog.code_until_ret(a)
            if not end:
                raise SystemExit('--entries: %#x does not decode as code up to a ret' % a)
            prog.functions[a] = [[a, end]]
            prog.entries.add(a)
    prog.prefix = args.module + '_' if args.module else 'f_'
    for h in filter(None, args.hooks.split(',')):
        name, addr = h.split('=')
        prog.hooks[int(addr, 16)] = name
    for w in filter(None, args.wraps.split(',')):
        name, addr = w.split('=')
        a = int(addr, 16)
        if a not in prog.entries:
            raise SystemExit('--wraps: %s=%#x is not a function entry in the metadata' % (name, a))
        prog.wraps[a] = name
    patched = {}  # function entry -> [groups that patch it]
    if args.patches:
        with open(args.patches) as f:
            for group, spots in json.load(f).items():
                if not group.replace('_', '').isalnum():
                    raise SystemExit('--patches: %r is not a C identifier' % group)
                prog.patches[group] = {}
                for addr, hexbytes in spots.items():
                    a = int(addr, 16)
                    data = bytes.fromhex(hexbytes)
                    for i, v in enumerate(data):
                        prog.patches[group][a + i] = v
                    e = prog.function_of(a)
                    if e is None:
                        raise SystemExit('--patches: %s at %#x is in no function' % (group, a))
                    if group not in patched.setdefault(e, []):
                        patched[e].append(group)

    if args.all:
        todo = sorted(prog.entries)
        closure = False
    else:
        todo = [int(x, 16) for x in args.functions.split(',') if x]
        closure = True
        for e in todo:
            if e not in prog.entries:
                raise SystemExit('%#x is not a function entry in the metadata' % e)

    prog.functions_at_start = set(prog.functions)
    done = {}
    flaginfo = {}
    reginfo = {}
    unimpl = collections.Counter()
    unimpl_funcs = set()
    queue = list(todo)
    progress = os.environ.get('XIV_PROGRESS')  # "@progress translate <done> <total>" lines (tools/xiv_run.py)
    while queue:
        e = queue.pop()
        if e in done:
            continue
        prog.referenced = set()
        t = FunctionTranslator(prog, e, prog.functions[e])
        done[e] = t.translate()
        if progress and len(done) % 250 == 0:
            print('@progress translate %d %d' % (len(done), len(done) + len(queue)), flush=True)
        flaginfo[e] = t.flaginfo
        reginfo[e] = t.reginfo
        variants = []
        for group in patched.get(e, []):
            prog.overlay = prog.patches[group]
            v = FunctionTranslator(prog, e, prog.functions[e]).translate()
            prog.overlay = None
            variants.append((group, v))
        if e in prog.wraps or variants:
            done[e] = gate(prog, e, done[e], variants)
            reginfo[e] = dict(reginfo[e], unsafe=True)  # the host or a patch decides what it reads
        for addr, mn, why in t.unimpl:
            unimpl[why if why.startswith('x87') else mn] += 1
            unimpl_funcs.add(e)
        # entries found while translating (x86c.py: calls to code no function starts at) are
        # translated too, in --all as well as for a closure
        queue.extend(r for r in prog.referenced if r not in done and (closure or r not in prog.functions_at_start))

    # Flags across calls (x86c.py flag_analysis): a call and a return pass only the registers,
    # except where flags really cross - a callee whose flags a caller reads returns them (and so,
    # in turn, do the callees and tail-jump targets its returned flags come from), and a callee that
    # reads flags on entry gets its callers'.
    flags_out = set()
    indirect_live = 0
    # callees that read flags on entry, as far as each function's own liveness says (either mode:
    # whether it returns flags does not change what it reads first)
    reads_in = {e for e, fi in flaginfo.items() if fi[0]['entry']}
    changed = True
    while changed:
        changed = False
        indirect_live = 0
        for e, fi in flaginfo.items():
            m = 1 if e in flags_out else 0
            for site, tgt, la, nxt_call in fi[m]['calls']:
                if nxt_call is not None and nxt_call in reads_in:
                    la = True  # its flags go on into a callee that reads them
                if not la:
                    continue
                if tgt is None:
                    indirect_live += 1
                    if not changed and os.environ.get('RECOMP_FLAGS_DEBUG'):
                        print('flags live after indirect call at %#x (in %#x)' % (site, e), file=sys.stderr)
                elif tgt not in flags_out:
                    flags_out.add(tgt)
                    changed = True
            if m:
                for tgt in fi['tails']:
                    if tgt not in flags_out:
                        flags_out.add(tgt)
                        changed = True
    flags_in = {e for e, fi in flaginfo.items() if fi[1 if e in flags_out else 0]['entry']}
    print('flags across calls: %d functions return flags, %d read them on entry, %d indirect calls with '
          'flags live after' % (len(flags_out), len(flags_in), indirect_live), file=sys.stderr)
    to_re = re.compile(r'REGS_STORE_TO_([0-9a-f]{8});')

    def to_store(mm):
        return 'REGS_STORE;' if int(mm.group(1), 16) in flags_in else 'REGS_STORE_NOFLAGS;'
    bisect = os.environ.get('RECOMP_FLAGS_BISECT')  # the skipped stores decided at run time (XIV_FLAGCUT)
    if bisect:
        def to_store(mm):  # noqa: F811
            t = int(mm.group(1), 16)
            return 'REGS_STORE;' if t in flags_in else 'REGS_STORE_CUT(0x%08x);' % t
    # RECOMP_REGS=1: not yet run in game, so off by default
    regs_mod, regs_in, regs_ret = register_liveness(reginfo) if os.environ.get('RECOMP_REGS') and not bisect else ({}, {}, {})
    call_re = re.compile(r'REGS_STORE_TO_([0-9a-f]{8});(.*?)REGS_LOAD;')

    def call_regs(mm):
        t = int(mm.group(1), 16)
        if t not in regs_in:
            return to_store(mm) + mm.group(2) + 'REGS_LOAD;'
        return regs_store(regs_in[t], t in flags_in) + mm.group(2) + regs_load(regs_mod[t])
    for e in done:
        if e in regs_mod:
            ret = regs_store(regs_ret[e], e in flags_out)
        else:
            ret = 'REGS_STORE;' if e in flags_out else ('REGS_STORE_CUT(0x%08x);' % e if bisect else 'REGS_STORE_NOFLAGS;')
        done[e] = [to_re.sub(to_store, call_re.sub(call_regs, ln)).replace('REGS_STORE_RET;', ret)
                   if isinstance(ln, str) else ln for ln in done[e]]

    os.makedirs(args.out, exist_ok=True)
    entries = sorted(done)
    # A second module relocates independently of FFXiMain: its translation reads its own delta.
    delta = ('\n#undef RD\n#define RD rt_delta_%s\nextern uint32_t rt_delta_%s;\n' % (args.module, args.module)
             if args.module else '')
    with open(os.path.join(args.out, 'funcs.h'), 'w') as f:
        f.write('/* generated by recomp.py - do not edit, do not commit */\n#pragma once\n#include "guest.h"\n%s\n' % delta)
        f.write('/* imports inlined at their calls (x86c.py INLINE_IMPORTS; runtime/portable/k32.c) */\n'
                'uint64_t rt_monotonic_ns(void);\nvoid xi_cs_enter(uint32_t cs);\nvoid xi_cs_leave(uint32_t cs);\n'
                'uint32_t xi_tls_get(uint32_t i);\nuint32_t xi_tls_set(uint32_t i, uint32_t v);\n'
                'uint32_t xi_get_error(void);\nvoid xi_set_error(uint32_t e);\nuint32_t xi_wait1(uint32_t h, uint32_t ms);\n'
                'uint32_t xi_set_event(uint32_t h, int state);\nuint32_t xi_release_semaphore(uint32_t h, uint32_t n, uint32_t prev);\n'
                '#define XI_P(a) ((volatile uint32_t*)GUEST_PTR(a))\n\n')
        for e in entries:
            f.write('void %s%08x(Guest* g);\n' % (prog.prefix, e))
        for name in sorted(prog.hooks.values()):
            f.write('extern GuestFn rt_hook_%s;\n' % name)
        for e, name in sorted(prog.wraps.items()):
            f.write('void %s%08x_body(Guest* g);\nextern GuestFn rt_wrap_%s;\nextern const GuestFn rt_orig_%s;\n'
                    % (prog.prefix, e, name, name))
        for e, groups in sorted(patched.items()):
            f.write('void %s%08x_sel(Guest* g);\n' % (prog.prefix, e))
            if e not in prog.wraps:
                f.write('void %s%08x_body(Guest* g);\n' % (prog.prefix, e))
            for group in groups:
                f.write('void %s%08x_p_%s(Guest* g);\n' % (prog.prefix, e, group))
        for group in sorted(prog.patches):
            f.write('extern volatile int rt_patch_%s;\n' % group)
    chunks = set()
    for k in range(0, len(entries), args.chunk):
        name = 'funcs_%03d.c' % (k // args.chunk)
        chunks.add(name)
        text = ('/* generated by recomp.py - do not edit, do not commit */\n#include "funcs.h"\n\n'
                '#if defined(_MSC_VER)\n#pragma warning(disable: 4102 4189 4101 4702)\n'
                '#pragma code_seg(".xlat") /* translated code in its own section: the profiler tells it from the runtime */\n'
                '#endif\n\n')
        text += ''.join('\n'.join(done[e]) + '\n\n' for e in entries[k:k + args.chunk])
        write_if_changed(os.path.join(args.out, name), text)
    for old in os.listdir(args.out):
        if old.startswith('funcs_') and old.endswith('.c') and old not in chunks:
            os.remove(os.path.join(args.out, old))
    missed = sorted(set(prog.hooks) - prog.hooked)
    if missed and args.all:
        raise SystemExit('--hooks: %s is not an instruction in any translated function'
                         % ', '.join('%s=%#x' % (prog.hooks[a], a) for a in missed))
    kinds = patch_kinds(prog, entries)
    img = image_constants(prog, args.retail or retail_dll())
    with open(os.path.join(args.out, 'table.c'), 'w') as f:
        f.write('/* generated by recomp.py - do not edit, do not commit */\n#include "runtime.h"\n#include "funcs.h"\n\n')
        if args.module:
            # A second module: one descriptor the runtime registers (rt_add_module), and its delta.
            m = args.module
            f.write('uint32_t rt_delta_%s;\n\nstatic const RtEntry table[] = {\n' % m)
            for e in entries:
                f.write('    { 0x%08Xu, %s%08x },\n' % (e, prog.prefix, e))
            f.write('};\n\n/* The pinned retail build this translation belongs to. */\n')
            f.write('const RtModule rt_module_%s = {\n    "%s", table, %d, &rt_delta_%s,\n' % (m, m, len(entries), m))
            f.write('    ' + ', '.join('0x%Xu' % img[k] for k in ('base', 'timestamp', 'size', 'text_rva', 'text_size',
                                                                     'packed_rva', 'packed_len', 'oep', 'reloc_rva')))
            f.write(',\n};\n')
            print('module %s: translated %d functions (%d with unimplemented instructions)' % (m, len(entries), len(unimpl_funcs)))
            if args.stats or unimpl:
                for k, v in unimpl.most_common(40):
                    print('  %7d  %s' % (v, k))
            return
        for name in sorted(prog.hooks.values()):
            f.write('GuestFn rt_hook_%s; /* recomp.py --hooks */\n' % name)
        for e, name in sorted(prog.wraps.items()):
            orig = 'f_%08x_sel' % e if e in patched else 'f_%08x_body' % e
            f.write('GuestFn rt_wrap_%s; /* recomp.py --wraps */\nconst GuestFn rt_orig_%s = %s;\n' % (name, name, orig))
        for group in sorted(prog.patches):
            f.write('volatile int rt_patch_%s; /* recomp.py --patches */\n' % group)
        # the patches, one entry per run of consecutive bytes, for the host to compare with memory
        f.write('static const unsigned char rt_patch_bytes[] = {')
        patch_runs, off = [], 0
        blob = []
        for group in sorted(prog.patches):
            spots = sorted(prog.patches[group].items())
            k = 0
            while k < len(spots):
                start, run = spots[k][0], [spots[k][1]]
                while k + 1 < len(spots) and spots[k + 1][0] == spots[k][0] + 1:
                    k += 1
                    run.append(spots[k][1])
                patch_runs.append((group, start, off, len(run)))
                blob += run
                off += len(run)
                k += 1
        f.write(','.join('0x%02x' % b for b in blob) or '0')
        f.write('};\nconst RtPatch rt_patches[] = {\n')
        for group, start, o, n in patch_runs:
            f.write('    { "%s", 0x%08Xu, rt_patch_bytes + %d, %d, &rt_patch_%s },\n' % (group, start, o, n, group))
        if not patch_runs:
            f.write('    { 0, 0, 0, 0, 0 },\n')
        f.write('};\nconst unsigned rt_patch_count = %d;\n' % len(patch_runs))
        if prog.hooks or prog.wraps:
            f.write('\n')
        f.write('const RtEntry rt_table[] = {\n')
        for e in entries:
            f.write('    { 0x%08Xu, f_%08x },\n' % (e, e))
        f.write('};\nconst unsigned rt_table_count = %d;\n\n' % len(entries))
        f.write('/* How each entry is redirected into its translation: 0 = 5-byte jmp, 1 = int3 (too close\n'
                ' * to the next entry, or a body shorter than 5 bytes with no padding after it). */\n')
        f.write('const unsigned char rt_table_patch[] = {\n')
        for k in range(0, len(kinds), 32):
            f.write('    ' + ','.join(str(x) for x in kinds[k:k + 32]) + ',\n')
        f.write('};\n\n')
        f.write('/* The pinned retail build this translation belongs to. */\n')
        for name, value in img.items():
            f.write('const uint32_t rt_image_%s = 0x%Xu;\n' % (name, value))
    print('entry patches: %d jmp, %d int3' % (kinds.count(0), kinds.count(1)))

    print('translated %d functions (%d with unimplemented instructions)' % (len(entries), len(unimpl_funcs)))
    if args.stats or unimpl:
        total = sum(unimpl.values())
        print('unimplemented instructions: %d' % total)
        for k, v in unimpl.most_common(40):
            print('  %7d  %s' % (v, k))


if __name__ == '__main__':
    main()
