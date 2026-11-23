"""Lift the SGX audio SPURS jobs at their observed local-store addresses.

These are position-independent job binaries, not the C0DEC0DE WWS jobs.
Their +0x10/+0x18 words are executable entry stubs, not an entry offset.
The SDK PPU descriptors at 0x016B9378 supply name/source EA/size. PDI's four
descriptors at 0x016B7F64 leave size zero; their bounds come from captured GETs.
Runtime GET captures establish the load addresses below; unsupported addresses continue
through the interpreter. Every registered body is verified after DMA.

Usage: python tools/lift_audio_module.py [--load-trace logs/new_capture.txt]
       python tools/lift_audio_module.py --pm-only
       python tools/lift_audio_module.py --registration-only
       python tools/lift_audio_module.py --optimize-existing
The optional trace adds exact (source EA, LS address) pairs to the known set.
The independent low JobChain policy comes from libsre.prx, not the WWS manager.
"""
from pathlib import Path
from game_paths import GAME
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / "ps3recomp" / "tools"
sys.path.insert(0, str(KIT))
from wrap_spu_elf import wrap
from lift_jobmods import module_code_end, module_seeds

ELF = GAME / "EMAIN.ELF"
OUT = ROOT / "spu_audio"
AUDIO_PM_IMAGE_ID = 334  # Reserved independently of codec/DSP load variants.


def write_if_changed(path, content):
    encoded = content.encode("utf-8") if isinstance(content, str) else content
    if not path.exists() or path.read_bytes() != encoded:
        path.write_bytes(encoded)


# Three measured synth loops only; hashes fail closed if the upstream lifter
# changes their control flow. Original, local-GPR and RZ-specialized forms are
# accepted so refreshing translations is idempotent. Guest bytes stay exact.
AUDIO_LOCAL_VARIANT = "gt6_socisynth_04c00"
AUDIO_LOCAL_BODIES = {
    0x9318: ("8ee7643fe50fd20dea7989b62f47c0acf13dfd9bf68c9fc226358d83ea04a4ad",
             "c5f6459fb145452eb4a51e455d3861955f778683b802fbee09f17bfc948309d7"),
    0x9878: ("4f2f5658bdc8be75ae3ab82eacb72d1182bd131dcfd8ff8457097c7b065df89f",
             "99ca2946af757a698d5ea31d0ff5359c2af3bd304602f90c6b5a15718f28ed67"),
    0x90C4: ("f6d2112fdbcbd94aad2ab2b2c3cabfe462fdb853ad7b81e3c2b8692c9676e64f",
             "35c74ad8110e9acd2d127ec01e4b1ddbd7fd9c177723d9e673451c4cb3b46546"),
}
AUDIO_RZ_BODIES = {
    0x9318: "0fd9b9b0537162708abb17c16765d2928e024bc3e9d91ba6d110666f648be8a3",
    0x9878: "daaa607dda9172c34e4c627d0f4e5003528cd9146786e07932871acc3fd48933",
    0x90C4: "24038a69e6624e0e36406d3db75856fd38c6597aaeb8ade0939186bb3ee62b6b",
}

# Eight further leaves measured from verified native-entry LS/GPR captures.
# Each tuple is original/local/final body SHA256 and the selected strategy.
# Distortion keeps its original fallback because local caching regressed RZ-off.
AUDIO_EXTRA_BODIES = {
    "gt6_socedly_17400": {
        0x1CED8: ("4b9c22a6f4513983ad6c0469d49048c7c8e9d49c190b0b6c59709dab18586ca8",
                  "f3b1457d010dd2f0da920862769bc7814ac38fa77e43fd9a32051bcbe817b02e",
                  "33ad89ec700e81bea0799cd6139879e0046ec55c5838bb316ccd03b25123e76b", "combined"),
        0x196D0: ("779d06640b0569a50efffd904b11467b7500c9ab8a3b5601388cdc975a2a29fa",
                  "f424b2a5c7cb179c894fc3d020f5a61157ae74237d98766ca98e265de34a7ad3",
                  "bf6584b0c0882191d3725709e57f72a6e8a1f5063fd1763865e4ff618e39ec3b", "combined"),
    },
    "gt6_socisynth_04c00": {
        0x6500: ("3cc48a1c8e99badfcc99e058fedb42140c588c65bf9193958a6f6221c0d801d1",
                 "b4c932dc5750e1c291cf19a6b9343cc81ad70ce3a53d406015e13b3b59c7713a",
                 "6f16f6d78122d2a618c580e8985c17319920966b0583ca014ca093fa2ca8e434", "combined"),
        0x6A80: ("85adab60b7d3008f760075a672b52dfbb1c8cad2c63a6fa0560b2b9c9203d93a",
                 "080bb358c11132a56fad84c1b7b34e69caa63d0a056e55da87ac38f90a8baf08",
                 "dbb833a1fc74ec2bfcdb226d0512b15c46d338c1cbb3ced7af6cab965730a3e3", "combined"),
        0x9A50: ("40360287e96a3e46f312479888b5df11094aa272214e9105df18b694633e7c8e",
                 "7ed883084c4b1faed08c96e88ff3ba22610fd6174a56532a93ceb7e2dea20815",
                 "8f09fc83646ea8cd79f3789a712797f6e2ad80caaf0e739121d4704fc604829a", "combined"),
    },
    "gt6_socepdidist_18c00": {
        0x19598: ("bb66928862b5eed90d883d75ff91432ad654ca23539005162d47fd5b5409ac03",
                  "ceb663ceb989f4f1ef3950612766a6dad4a1bae23b2915ee4afe1ad5aa30fe52",
                  "829b16047c9bba33c66da1a679c6351ab23b6f08014d33ea8e44447d9f7296a7", "rz-local-only"),
    },
    "gt6_socetverb_04c00": {
        0x8320: ("c3af7cf93092c5cf8596d93fd3c6c22e469d760637b2608f29745bde30a04e73",
                 "fb977b1fc827b61d95972ccbd8cf4cf302caaacbe92ac68937937a6822d72205",
                 "f960a3e7d90463ad925a09c68f96422bbe70af5d130588d202e2c8c01d3fd6cc", "combined"),
    },
    "gt6_socefilter_12c00": {
        0x12DC0: ("6e5182103a2ad4a0eae6134a7d7727087bd9f78ddef27dcf6f4c59fa6ca0d97c",
                  "cd4966db950193357fbdb3986d7cbf31971fc5ec64d0bc326a2bd04277dbc80c",
                  "4d55ec029eb7bc8a0c9fa7ae26b396f5af3d9fc62af518ec71cc50087b68d915", "combined"),
    },
}


def promote_audio_leaf(body):
    """Cache r5+ until the leaf's static exit; retain observable r0-r4.

    These bodies call only pure register helpers, LS read128/write128 and
    SPU_DIRECT. The LS diagnostics inspect r0-r4, which remain in ctx. The
    code-write callback modifies LS residency/cache/bitmap and no GPRs. No
    channel, IRQ, guest call, return or non-local transfer occurs inside a loop.
    Every static exit commits all changed cached registers before dispatch.
    This is a title-local transform, not a general lifter optimization.
    """
    regs = sorted({int(x) for x in re.findall(r'ctx->gpr\[(\d+)\]', body) if int(x) >= 5})
    writes = sorted({int(x) for x in re.findall(r'ctx->gpr\[(\d+)\]\s*=', body) if int(x) >= 5})
    forbidden = ('spu_rdch', 'spu_wrch', 'spu_rchcnt', 'SPU_RET', 'SPU_INDIRECT', 'SPU_CALL')
    if any(x in body for x in forbidden) or 'SPU_DIRECT(' not in body:
        raise ValueError('audio local-GPR candidate is not a supported leaf')
    cached = re.sub(r'ctx->gpr\[(\d+)\]', lambda m: f'gpr_{m[1]}' if int(m[1]) >= 5 else m[0], body)
    brace = cached.index('{') + 1
    init = ''.join(f'\n    u128 gpr_{r} = ctx->gpr[{r}];' for r in regs) + '\n'
    cached = cached[:brace] + init + cached[brace:]
    flush = ' '.join(f'ctx->gpr[{r}] = gpr_{r};' for r in writes)
    cached = re.sub(r'SPU_DIRECT\(([^;]+)\);', lambda m: '{ ' + flush + ' SPU_DIRECT(' + m[1] + '); }', cached)
    return cached


def hoist_audio_rz(body):
    """Test RZ once in reviewed leaves, keeping the exact existing helpers.

    No guest call, channel, FP control write or new host frame occurs inside
    these bodies. Their LS diagnostic and residency callbacks preserve FP
    control. The existing extended-SPU helpers handle exceptional bit patterns
    unchanged. Non-RZ, non-AVX2/FMA and scalar builds retain the original body.
    Every fast-path exit is the same static SPU_DIRECT, including GPR flushes.
    """
    start, end = body.index('{')+1, body.rfind('}')
    inner = body[start:end]

    def fp(match):
        op, args = match.groups()
        if op == 'fm':
            return f'spu__xr_fm({args})'
        if op == 'fa':
            return f'spu__xr_fa({args}, 0)'
        if op == 'fma':
            return f'spu__xr_fma({args}, 0, 0)'
        if op == 'fnms':
            return f'spu__xr_fma({args}, 0x80000000u, 0)'
        raise ValueError(op)

    fast, count = re.subn(r'spu_(fm|fa|fma|fnms)\(([^()]*)\)', fp, inner)
    if not count or re.search(r'spu_(?:fm|fa|fma|fnms)\(', fast):
        raise ValueError('audio RZ candidate has unsupported floating-point operands')
    fast = re.sub(r'\bloc_([0-9A-F]+)\b', r'rz_loc_\1', fast)
    return body[:start] + '''
#if defined(__AVX2__) && defined(__FMA__) && !defined(SPU_SCALAR_HELPERS)
    /* No channel or FP-mode-changing call occurs inside this reviewed leaf. */
    if (spu__x8_rz()) {
''' + fast + '''
    }
#endif
''' + body[start:]


def hoist_audio_rz_fast_locals(body):
    """Cache GPRs only inside the RZ fast path; leave the fallback verbatim."""
    combined = hoist_audio_rz(promote_audio_leaf(body))
    split = combined.index('\n#endif\n') + len('\n#endif\n')
    return combined[:split] + body[body.index('{')+1:]


AUDIO_AVX512_ENTRIES = {
    'gt6_socisynth_04c00': {0x9318, 0x9878, 0x90C4, 0x6500, 0x6A80},
    'gt6_socedly_17400': {0x1CED8, 0x196D0},
    'gt6_socepdidist_18c00': {0x19598},
    'gt6_socetverb_04c00': {0x8320},
    'gt6_socefilter_12c00': {0x12DC0},
}


def audio_multiversion_body(body, name):
    """Clone a hash-verified body; retain the original registered symbol.

    Only the AVX-512 clone changes target features. Its ISA gate is cached at
    startup and includes OSXSAVE/XCR0. The original AVX2 body is retained.
    Both wrapper exits are tail calls, preserving the SPU host-frame contract.
    """
    token = 'void ' + name + '('
    if not body.startswith(token):
        raise ValueError('audio multiversion body does not start at its entry')
    avx2 = body.replace(token, 'static GT6_AUDIO_NOINLINE void ' + name + '__avx2(', 1)
    avx512 = body.replace(token, 'static GT6_AUDIO_AVX512_TARGET void ' + name + '__avx512(', 1)
    return (f'/* GT6_AUDIO_AVX512_BEGIN {name} */\n' + avx2 +
            '#if GT6_AUDIO_HAS_AVX512\n' + avx512 + '#endif\n' +
            f'void {name}(spu_context* ctx) {{\n'
            '#if GT6_AUDIO_HAS_AVX512\n'
            f'    if (g_gt6_audio_avx512) {{ GT6_AUDIO_TAIL({name}__avx512, ctx); }}\n'
            '#endif\n'
            f'    GT6_AUDIO_TAIL({name}__avx2, ctx);\n'
            '}\n' + f'/* GT6_AUDIO_AVX512_END {name} */\n\n')


def unwrap_audio_multiversion(text, name, expected_hash):
    start_marker = f'/* GT6_AUDIO_AVX512_BEGIN {name} */\n'
    if start_marker not in text:
        return text
    end_marker = f'/* GT6_AUDIO_AVX512_END {name} */\n\n'
    start = text.index(start_marker)
    end = text.index(end_marker, start) + len(end_marker)
    region = text[start:end]
    token = 'static GT6_AUDIO_NOINLINE void ' + name + '__avx2('
    body_start = region.index(token)
    body_end = region.index('#if GT6_AUDIO_HAS_AVX512\n', body_start)
    body = region[body_start:body_end].replace(token, 'void ' + name + '(', 1)
    if hashlib.sha256(body.encode()).hexdigest() != expected_hash or region != audio_multiversion_body(body, name):
        raise ValueError(f'audio multiversion body changed; review before optimizing: {name}')
    return text[:start] + body + text[end:]


def apply_audio_local_gprs(variant):
    reviewed = {}
    if variant == AUDIO_LOCAL_VARIANT:
        reviewed.update({entry: (before, local, AUDIO_RZ_BODIES[entry], 'combined')
                         for entry, (before, local) in AUDIO_LOCAL_BODIES.items()})
    reviewed.update(AUDIO_EXTRA_BODIES.get(variant, {}))
    if not reviewed:
        return
    path = OUT / variant / 'spu_recomp.c'
    text = path.read_text()
    # Unwrap every selected region before locating ordinary function bounds.
    # Adjacent selected entries (6500/6A80) otherwise hide the next declaration
    # behind their static clones. Stage all replacements until validation ends.
    for entry, (_, _, rz_hash, _) in reviewed.items():
        if entry in AUDIO_AVX512_ENTRIES.get(variant, ()):
            text = unwrap_audio_multiversion(text, f'{variant}_spu_func_{entry:08X}', rz_hash)
    records = []
    replacements = []
    for entry, (before_hash, local_hash, rz_hash, strategy) in reviewed.items():
        name = f'{variant}_spu_func_{entry:08X}'
        multiversion = entry in AUDIO_AVX512_ENTRIES.get(variant, ())
        start = text.index('void ' + name + '(')
        end = text.index('\nvoid ', start + 1)
        body = text[start:end]
        digest = hashlib.sha256(body.encode()).hexdigest()
        if digest == before_hash:
            if strategy == 'rz-local-only':
                body = hoist_audio_rz_fast_locals(body)
                digest = hashlib.sha256(body.encode()).hexdigest()
                if digest != rz_hash:
                    raise ValueError(f'RZ-only local-GPR transform changed unexpectedly: {name}')
            else:
                body = promote_audio_leaf(body)
                digest = hashlib.sha256(body.encode()).hexdigest()
                if digest != local_hash:
                    raise ValueError(f'local-GPR transform changed unexpectedly: {name}')
        if digest == local_hash and strategy == 'combined':
            body = hoist_audio_rz(body)
            digest = hashlib.sha256(body.encode()).hexdigest()
            if digest != rz_hash:
                raise ValueError(f'RZ-entry transform changed unexpectedly: {name}')
        elif digest != rz_hash:
            raise ValueError(f'audio leaf changed; review before optimizing: {name}')
        final_body = audio_multiversion_body(body, name) if multiversion else body
        replacements.append((start, end, final_body))
        records.append(dict(function=name, entry=entry, source_body_sha256=before_hash,
                            local_body_sha256=local_hash, optimized_body_sha256=rz_hash,
                            strategy=strategy, avx512_multiversion=multiversion,
                            emitted_body_sha256=hashlib.sha256(final_body.encode()).hexdigest()))
    for start, end, body in sorted(replacements, reverse=True):
        text = text[:start] + body + text[end:]
    if AUDIO_AVX512_ENTRIES.get(variant) and '#include "../audio_isa.h"' not in text:
        text = text.replace('#include "spu_recomp.h"\n', '#include "spu_recomp.h"\n#include "../audio_isa.h"\n', 1)
    write_if_changed(path, text)
    manifest = OUT/'audio_locals_manifest.json' if variant == AUDIO_LOCAL_VARIANT else path.parent/'leaf_optimizations.json'
    write_if_changed(manifest, json.dumps(dict(
        variant=variant, functions=records, direct_context_registers=list(range(5)),
        contract=promote_audio_leaf.__doc__.strip(),
        rz_contract=hoist_audio_rz.__doc__.strip()), indent=2)+'\n')

# norm_audio_capture02: distinct first GETs, not continuation chunks. The full
# socdac3@4C00, socisynth@4C00, soceumix@12000 and socedly@17400 captures also
# match their EMAIN payloads byte for byte.
BASES = {
    "socdac3": [0x4C00],
    "socisynth": [0x4C00, 0x18C00],
    "socedmix": [0x4C00, 0x10400, 0x12000],
    "socethru": [0x4C00, 0x10400],
    "soceamp": [0x4C00, 0xC800],
    "socefilter": [0x4C00, 0x12C00],
    "socecomp": [0x4C00, 0x8400, 0x10000],
    "socelverb": [0x4C00],
    "socedly": [0x4C00, 0x17400],
    "soceumix": [0x4C00, 0x12000],
    "socelverbx": [0x4C00],
    "socetverb": [0x4C00, 0x10800, 0x18400, 0x29000],
    "socepdicomp": [0x4C00, 0xCC00, 0x14800],
    "socepdideq": [0x4C00, 0x1C400],
    "socepdidist": [0x4C00, 0x18C00],
    "socepdirta": [0x4C00],
}

# norm_freerun01 expanded GET trace. These are the exact contiguous DMA spans,
# including any 16-byte DMA padding; no size is guessed from the next module.
PDI_MODULES = {
    "socepdicomp": (0x016B7F64, 0x01417D00, 0x3970),
    "socepdideq": (0x016B7F70, 0x01410180, 0x7B70),
    "socepdidist": (0x016B7F7C, 0x0140CD00, 0x3410),
    "socepdirta": (0x016B7F88, 0x01406E80, 0x5E80),
}


def read_modules(elf):
    phoff, = struct.unpack_from(">Q", elf, 0x20)
    phsize, phnum = struct.unpack_from(">HH", elf, 0x36)
    segments = []
    for i in range(phnum):
        p = phoff + i * phsize
        kind, = struct.unpack_from(">I", elf, p)
        off, ea = struct.unpack_from(">QQ", elf, p + 8)
        size, = struct.unpack_from(">Q", elf, p + 0x20)
        if kind == 1:
            segments.append((off, ea, size))

    def offset(ea):
        for off, start, size in segments:
            if start <= ea < start + size:
                return off + ea - start
        raise ValueError(f"EA 0x{ea:X} is outside file-backed PT_LOAD segments")

    modules = []
    for i, expected in enumerate(BASES):
        pdi = PDI_MODULES.get(expected)
        desc = pdi[0] if pdi else 0x016B9378 + i * 12
        name_ea, ea, size = struct.unpack_from(">III", elf, offset(desc))
        if pdi:
            if ea != pdi[1] or size != 0:
                raise ValueError(f"unexpected PDI descriptor at 0x{desc:X}")
            size = pdi[2]
        pos = offset(name_ea)
        name = elf[pos:elf.index(b"\0", pos)].decode("ascii")
        if name != expected or not 0x30 < size < 0x40000:
            raise ValueError(f"unexpected SGX descriptor at 0x{desc:X}")
        off = offset(ea)
        raw = elf[off:off + size]
        if len(raw) != size or raw[0x10:0x20] != bytes.fromhex(
                "44012850320000804401285032000280"):
            raise ValueError(f"unexpected SPURS job entry for {name}")
        modules.append(dict(name=name, ea=ea, size=size, raw=raw,
                            sha256=hashlib.sha256(raw).hexdigest(),
                            bases=set(BASES[name])))
    return modules


def lift_audio_pm():
    """Lift the byte-identical JobChain policy observed in four audio captures.

    LS1D4 in the live primary kernel points to runtime EA3002A180. The PRX
    PT_LOAD mapping puts those bytes at file2A280. No data region is lifted:
    A00..46A0 is stable across init, DSP playback, and a twelve-car race.
    """
    prx_path = GAME / 'firmware/libsre.prx'
    prx = prx_path.read_bytes()
    if prx[:6] != b'\x7fELF\x02\x02':
        raise ValueError('libsre must be the expected big-endian ELF64 PRX')
    phoff, = struct.unpack_from('>Q', prx, 0x20)
    phsize, phnum = struct.unpack_from('>HH', prx, 0x36)
    source_va, source_ea = 0x2A180, 0x3002A180
    base, end, image_id = 0xA00, 0x46A0, AUDIO_PM_IMAGE_ID
    span = end - base
    off = None
    for i in range(phnum):
        p = phoff + i * phsize
        kind, = struct.unpack_from('>I', prx, p)
        file_pos, va = struct.unpack_from('>QQ', prx, p + 8)
        size, = struct.unpack_from('>Q', prx, p + 0x20)
        if kind == 1 and va <= source_va and source_va + span <= va + size:
            off = file_pos + source_va - va
            break
    if off is None:
        raise ValueError('JobChain policy is not wholly file-backed in libsre')
    code = prx[off:off+span]
    code_sha = hashlib.sha256(code).hexdigest()
    expected = '2f838c9ccf7fcda6cd81109d22b937763f7d2af5f64c88ec0893fbed06c15ca9'
    if len(code) != span or code_sha != expected:
        raise ValueError('libsre JobChain policy differs from the verified captures')
    name = 'gt6_audio_pm'
    folder = OUT / name
    folder.mkdir(parents=True, exist_ok=True)
    # Entry/reset points, observed interpreter returns, handler and kernel API
    # wrappers. 465C/4678 are ordinary returns, never stack-reset boundaries.
    # 2DFC..2E18 is a seven-word dispatch table used by 2D80; seed its
    # destinations, never the table address (whose first word decodes as STOP).
    dispatch_table = list(struct.unpack_from('>7I', code, 0x2DFC-base))
    seeds = [0xA00, 0xA10, 0xA34, 0x257C, 0x288C, 0x2B00,
             0x4610, 0x4620, 0x4638, 0x465C, 0x4678] + dispatch_table
    wrapped = OUT / (name + '.elf')
    report = OUT / (name + '_report.json')
    wrapped_bytes = wrap(code, base=base, entry=base)
    write_if_changed(wrapped, wrapped_bytes)
    # Auto discovery also seeds the word following an indirect branch. Here
    # that word is table data, so pass explicitly filtered real code bounds.
    from find_spu_functions import detect_functions
    bounds, _ = detect_functions(wrapped_bytes)
    bounds = [(a, min(b, 0x2DFC) if a < 0x2DFC < b else b)
              for a,b in bounds if not 0x2DFC <= a < 0x2E18]
    raw_path = OUT / (name + '.bin')
    bounds_path = OUT / (name + '_functions.json')
    write_if_changed(raw_path, code)
    write_if_changed(bounds_path, json.dumps([dict(start=a,end=b) for a,b in bounds],indent=2)+'\n')
    cmd = [sys.executable, str(KIT/'spu_lifter.py'), str(raw_path),
           '--functions', str(bounds_path), '--base', hex(base), '--code-end', hex(end),
           '--symbol-prefix', name+'_', '-o', str(folder),
           '--source-name', 'spu_recomp.c', '--header-name', 'spu_recomp.h',
           '--report', str(report), '--return-entries',
           '--extra-funcs', ','.join(hex(x) for x in seeds)]
    old = {}
    for path in (folder/'spu_recomp.c', folder/'spu_recomp.h', report):
        if path.exists():
            stat = path.stat()
            old[path] = (path.read_bytes(), stat.st_atime_ns, stat.st_mtime_ns)
    result = subprocess.run(cmd, capture_output=True, text=True)
    for path, (before, access, modified) in old.items():
        if path.read_bytes() == before:
            os.utime(path, ns=(access, modified))
    write_if_changed(OUT/(name+'_lift.log'), result.stdout+result.stderr)
    if result.returncode:
        raise RuntimeError('JobChain lifter failed: '+result.stderr[-2000:])
    info = json.loads(report.read_text())
    if info['unsupported'] or any(not base <= a < b <= end for a,b in info['functions']):
        raise ValueError('JobChain lift has unsupported instructions or invalid bounds')
    entries = {a for a,b in info['functions']}
    if not set(seeds) <= entries:
        raise ValueError('JobChain lift omitted a required observed entry')
    if any(0x2DFC <= entry < 0x2E18 for entry in entries):
        raise ValueError('JobChain data table was incorrectly offered as executable code')
    lines = ['/* GENERATED by tools/lift_audio_module.py: libsre audio JobChain policy. */',
             '#include "spu_overlay_residency.h"', '#include <stdint.h>',
             '#include <stdio.h>', '#include <stdlib.h>',
             'extern void spu_begin_image(int image_id);',
             'extern void spu_register_stack_reset_entry(uint32_t entry, int image_id);',
             'extern void gt6_audio_pm_spu_recomp_register(void);',
             'extern void gt6_audio_pm_register_kernel_yield(void);',
             'static const uint8_t code_body[] = {']
    for i in range(0,len(code),16):
        lines.append('    '+','.join(f'0x{x:02X}' for x in code[i:i+16])+',')
    lines += ['};', '', '__attribute__((constructor)) static void gt6_audio_pm_register_ctor(void)',
              '{', '    const char* e = getenv("GT6_AUDIO_PM_LIFT");',
              "    if (e && *e == '0') {",
              '        fprintf(stderr, "[gt6-audio-pm] GT6_AUDIO_PM_LIFT=0: JobChain policy interpreted\\n");',
              '        return;', '    }',
              '    const spu_overlay_code_segment code = {0xA00u, sizeof code_body, code_body};',
              '    spu_begin_image(334);', '    gt6_audio_pm_spu_recomp_register();',
              '    spu_begin_image(0);',
              '    if (!spu_overlay_register_verified_region(0x3002A180u, 0xA00u,',
              '            sizeof code_body, 334, &code, 1)) {',
              '        fprintf(stderr, "[gt6-audio-pm] JobChain region unavailable: interpreted\\n");',
              '        return;', '    }',
              '    spu_overlay_allow_unlifted_calls(334);',
              '    spu_register_stack_reset_entry(0xA00u, 334);',
              '    gt6_audio_pm_register_kernel_yield();',
              '    fprintf(stderr, "[gt6-audio-pm] verified JobChain policy LS A00-46A0, image 334\\n");',
              '}', '']
    write_if_changed(OUT/'audio_pm_register.c', '\n'.join(lines))
    write_if_changed(OUT/'audio_pm_manifest.json', json.dumps(dict(
        name=name, image_id=image_id, source=str(prx_path.relative_to(ROOT)),
        source_sha256=hashlib.sha256(prx).hexdigest(), source_file_offset=off,
        source_ea=source_ea, code_start=base, code_end=end, code_sha256=code_sha,
        functions=len(info['functions']), unsupported=info['unsupported'],
        explicit_entries=seeds, stack_reset_entries=[base],
        data_table_lsa=0x2DFC, data_table_targets=dispatch_table,
        excluded_data_ranges=[[0x2DFC,0x2E18]],
        kernel_yield_lsa=0x838, kernel_yield_primary_image=274060898,
        opt_out='GT6_AUDIO_PM_LIFT=0'), indent=2)+'\n')
    print(f'{name}: image {image_id}, code 0x{base:X}-0x{end:X}, '
          f"{len(info['functions'])} functions", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--load-trace", action="append", default=[])
    ap.add_argument("--pm-only", action="store_true", help="generate only the independent audio JobChain policy")
    ap.add_argument("--registration-only", action="store_true", help="refresh verified DSP/header registration without relifting bodies")
    ap.add_argument("--optimize-existing", action="store_true", help="apply the reviewed hash-checked DSP leaf transforms only")
    args = ap.parse_args()
    if args.optimize_existing:
        for variant in dict.fromkeys([AUDIO_LOCAL_VARIANT, *AUDIO_EXTRA_BODIES]):
            apply_audio_local_gprs(variant)
        return
    if args.pm_only:
        lift_audio_pm()
        return
    elf = ELF.read_bytes()
    modules = read_modules(elf)
    if args.registration_only:
        manifest = json.loads((OUT/'audio_manifest.json').read_text())['variants']
        by_name = {m['name']:m for m in modules}
        for v in manifest:
            m = by_name[v['module']]
            if v['ea'] != m['ea'] or v['sha256'] != m['sha256'] or v['size'] != m['size']:
                raise ValueError('existing DSP manifest differs from the current source bytes')
        write_audio_registration(modules, manifest)
        return
    if len({m["raw"][:16] for m in modules}) != len(modules):
        raise ValueError("SGX module headers must uniquely identify LS-copy candidates")
    by_ea = {m["ea"]: m for m in modules}
    previous = OUT / "audio_manifest.json"
    previous_ids = {}
    if previous.exists():
        for item in json.loads(previous.read_text())["variants"]:
            module = by_ea.get(item["ea"])
            if module and item["sha256"] == module["sha256"]:
                module["bases"].add(item["base"])
                previous_ids[item["name"]] = item["image_id"]
    if AUDIO_PM_IMAGE_ID in previous_ids.values():
        raise ValueError('a DSP image ID collides with the reserved audio JobChain policy')
    for path in args.load_trace:
        for line in Path(path).read_text(errors="replace").splitlines():
            hit = re.search(r"\[spu-get\].*ea=0x([0-9A-Fa-f]+) -> ls 0x([0-9A-Fa-f]+)", line)
            if hit:
                ea, base = (int(x, 16) for x in hit.groups())
                if ea in by_ea and base % 16 == 0 and base + by_ea[ea]["size"] <= 0x40000:
                    by_ea[ea]["bases"].add(base)

    OUT.mkdir(exist_ok=True)
    manifest = []
    next_image_id = max(previous_ids.values(), default=300) + 1
    for module in modules:
        raw = module["raw"]
        for base in sorted(module["bases"]):
            name = f"gt6_{module['name']}_{base:05x}"
            image_id = previous_ids.get(name)
            if image_id is None:
                if next_image_id == AUDIO_PM_IMAGE_ID:
                    next_image_id += 1
                image_id = next_image_id
                next_image_id += 1
            folder = OUT / name
            folder.mkdir(exist_ok=True)
            end = module_code_end(raw, name, base)
            if not end or end <= base + 0x30:
                raise ValueError(f"no reachable code for {name}")
            # This bound follows direct flow from +0x30 and reachable manual
            # return/switch entries. No data-tail entry is offered to the lifter.
            seeds = sorted({base + 0x10, base + 0x18, base + 0x30} |
                           {int(x, 0) for x in module_seeds(raw, base)
                            if base + 0x30 <= int(x, 0) < end})
            body = raw[0x10:end - base]
            wrapped = OUT / (name + ".elf")
            write_if_changed(wrapped, wrap(body, base=base + 0x10, entry=base + 0x10))
            report = OUT / (name + "_report.json")
            cmd = [sys.executable, str(KIT / "spu_lifter.py"), "--auto-functions",
                   str(wrapped), "--base", hex(base + 0x10), "--code-end", hex(end),
                   "--symbol-prefix", name + "_", "-o", str(folder),
                   "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h",
                   "--report", str(report), "--return-entries",
                   "--extra-funcs", ",".join(hex(x) for x in seeds)]
            # Adding one observed base should rebuild only that new translation.
            # The kit lifter writes in place, so retain timestamps of unchanged
            # sources after comparing the newly generated bytes.
            old = {}
            for source in (folder / "spu_recomp.c", folder / "spu_recomp.h", report):
                if source.exists():
                    stat = source.stat()
                    old[source] = (source.read_bytes(), stat.st_atime_ns, stat.st_mtime_ns)
            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode == 0:
                apply_audio_local_gprs(name)
            for source, (before, access, modified) in old.items():
                if source.read_bytes() == before:
                    os.utime(source, ns=(access, modified))
            write_if_changed(OUT / (name + "_lift.log"), result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError(f"lifter failed for {name}: {result.stderr[-2000:]}")
            info = json.loads(report.read_text())
            if info["unsupported"]:
                raise RuntimeError(f"unsupported instructions in {name}: {info['unsupported']}")
            manifest.append(dict(name=name, module=module["name"], image_id=image_id,
                                 ea=module["ea"], base=base, size=module["size"],
                                 code_start=base + 0x10, code_end=end,
                                 sha256=module["sha256"], functions=len(info["functions"]),
                                 unsupported=info["unsupported"]))
            print(f"{name}: image {image_id}, code 0x{base + 0x10:X}-0x{end:X}, "
                  f"{len(info['functions'])} functions", flush=True)

    write_audio_registration(modules, manifest)
    write_if_changed(OUT / "audio_manifest.json", json.dumps(dict(
        source_sha256=hashlib.sha256(elf).hexdigest(), variants=manifest), indent=2) + "\n")
    lift_audio_pm()


def write_audio_registration(modules, manifest):
    lines = ["/* GENERATED by tools/lift_audio_module.py; exact SGX audio code bytes. */",
             '#include "spu_overlay_residency.h"', '#include "spu_helpers.h"',
             '#include "audio_isa.h"',
             "#include <stdio.h>", "#include <stdlib.h>",
             "extern void spu_begin_image(int image_id);",
             "extern void spu_register_function(uint32_t lsa, void (*fn)(spu_context*));", ""]
    by_name = {m['name']:m for m in modules}
    headers = []
    for item in manifest:
        lines.append(f"extern void {item['name']}_spu_recomp_register(void);")
        body_entry = f"{item['name']}_spu_func_{item['base']+0x10:08X}"
        lines.append(f"extern void {body_entry}(spu_context*);")
        words = struct.unpack('>4I', by_name[item['module']]['raw'][:16])
        # The SGX content signature is executable: four ila r2,imm18 words.
        # A native policy calls the load base itself, not just the +0x10 stub.
        if any(w >> 25 != 0x21 or w & 0x7f != 2 for w in words):
            raise ValueError('SGX header is not four supported ila r2 instructions')
        header_entry = item['name']+'_header_entry'
        lines += [f'static void {header_entry}(spu_context* ctx)', '{']
        for w in words:
            lines.append(f'    ctx->gpr[2] = spu_ila(0x{(w>>7)&0x3ffff:X});')
        lines += [f'    SPU_DIRECT(ctx, 0x{item["base"]+0x10:X}, {body_entry});', '}', '']
        headers.append(dict(name=item['name'],image_id=item['image_id'],
                            header_entry=item['base'],body_entry=item['base']+0x10,
                            words=list(words)))
    for module in modules:
        item = next(m for m in manifest if m["module"] == module["name"])
        span = item["code_end"] - item["base"]
        if any(m["code_end"] - m["base"] != span for m in manifest if m["module"] == module["name"]):
            raise ValueError(f"inconsistent relocated code bound for {module['name']}")
        body = module["raw"][0x10:item["code_end"] - item["base"]]
        lines.append(f"static const unsigned char sig_{module['name']}[16] = {{")
        lines.append("    " + ",".join(f"0x{x:02x}" for x in module["raw"][:16]))
        lines.append("};")
        lines.append(f"static const unsigned char code_{module['name']}[] = {{")
        for i in range(0, len(body), 16):
            lines.append("    " + ",".join(f"0x{x:02x}" for x in body[i:i + 16]) + ",")
        lines.append("};")
    lines += ["", "__attribute__((constructor)) static void gt6_audio_register_ctor(void)",
              "{", '    const char* e = getenv("GT6_AUDIO_LIFT");',
              "    if (e && *e == '0') {",
              '        fprintf(stderr, "[gt6-audio] GT6_AUDIO_LIFT=0: SGX jobs interpreted\\n");',
              "        return;", "    }", "    gt6_audio_isa_init();", "    unsigned registered = 0, ls_copy = 0;"]
    for m in manifest:
        lines += [f"    spu_begin_image({m['image_id']});",
                  f"    {m['name']}_spu_recomp_register();",
                  f"    spu_register_function(0x{m['base']:X}u, {m['name']}_header_entry);",
                  "    {", "        const spu_overlay_code_segment code[2] = {",
                  f"            {{0x{m['base']:X}u, sizeof sig_{m['module']}, sig_{m['module']}}},",
                  f"            {{0x{m['code_start']:X}u, sizeof code_{m['module']}, code_{m['module']}}}",
                  "        };",
                  f"        if (spu_overlay_register_verified_region(0x{m['ea']:X}u, 0x{m['base']:X}u,",
                  f"                0x{m['code_end'] - m['base']:X}u, {m['image_id']}, code, 2)) {{",
                  "            ++registered;",
                  f"            ls_copy += spu_overlay_allow_ls_copy({m['image_id']}, sig_{m['module']});",
                  "        }", "    }"]
    lines += ["    spu_begin_image(0);",
              f'    fprintf(stderr, "[gt6-audio] verified SGX code: %u/{len(manifest)} load variants registered\\n", registered);',
              f'    fprintf(stderr, "[gt6-audio] LS copy verification: %u/{len(manifest)} load variants enabled\\n", ls_copy);',
              "}", ""]
    write_if_changed(OUT / "audio_register.c", "\n".join(lines))
    write_if_changed(OUT/'audio_header_manifest.json', json.dumps(dict(
        executable_headers=headers,verified_segments_per_variant=2,
        header_semantics='Four original ila r2 instructions, then tail transfer to the unchanged +0x10 entry.'),indent=2)+'\n')
    print(f'SGX registration: {len(headers)} verified executable headers and unchanged bodies',flush=True)


if __name__ == "__main__":
    main()
