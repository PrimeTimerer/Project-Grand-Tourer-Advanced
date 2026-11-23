"""Build fixture inputs from the hash-checked production leaf, without editing it."""
from pathlib import Path
import hashlib
import json
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import optimize_pdi_leaf as opt
sys.path.insert(0, str(ROOT / "ps3recomp/tools"))
from spu_disasm import disassemble_spu

OUT = ROOT / "scratch/latency_20261117/pdi_leaf/fixture"
OUT.mkdir(parents=True, exist_ok=True)
text = opt.SOURCE.read_text()
start, end, original, applied = opt.locate(text)
assert applied
artifact = opt.verify_artifact()
raw = bytes.fromhex(artifact["leaf_hex"])
rea_path = OUT.parent / "rea_original_bytes.json"
rea = json.loads(rea_path.read_text())
assert bytes(json.loads(rea["normalized_result"]["stdout"])) == raw
assert rea["normalized_result"]["artifact_changed"] is False

native = opt.transform_body(original)
prefix_original = original.replace("(spu_context* ctx)", "(spu_context* ctx, unsigned test_budget)", 1)
prefix_original = prefix_original.replace("        if (ctx->gpr[8]._u32[0] != 0)",
    "        if (--test_budget == 0) { SPU_RET(ctx); }\n        if (ctx->gpr[8]._u32[0] != 0)")
prefix_native = native.replace("(spu_context* ctx)", "(spu_context* ctx, unsigned test_budget)", 1)
flush = " ".join(f"ctx->gpr[{r}] = gpr_{r};" for r in [8] + list(range(21,39)))
prefix_native = prefix_native.replace("        if (gpr_8._u32[0] != 0)",
    f"        if (--test_budget == 0) {{ {flush} SPU_RET(ctx); }}\n        if (gpr_8._u32[0] != 0)")
prefix_native = prefix_native.replace("        if (ctx->gpr[8]._u32[0] != 0)",
    "        if (--test_budget == 0) { SPU_RET(ctx); }\n        if (ctx->gpr[8]._u32[0] != 0)")
assert prefix_original.count("--test_budget") == 1 and prefix_native.count("--test_budget") == 2
parts = [opt.GATE]
for label, body in [("original_14548", original), ("native_14548", native),
                    ("original_prefix_14548", prefix_original), ("native_prefix_14548", prefix_native)]:
    parts.append(body.replace("void " + opt.NAME, "__declspec(noinline) void " + label, 1))
parts.append("static const unsigned char original_opcodes[104] = {" +
             ",".join(f"0x{b:02x}" for b in raw) + "};\n")
(OUT / "pdi_leaf_cases.inc").write_text("\n".join(parts))
(OUT / "original_leaf.bin").write_bytes(raw)
(OUT / "original_leaf.asm").write_text("\n".join(str(x) for x in disassemble_spu(raw,0x14548))+"\n")

# Repeated application is exact, and neither a changed fallback nor a changed
# generated fast path can be silently overwritten. No production file is written.
assert text[start:end] == opt.region(original)
again = text[:start] + opt.region(original) + text[end:]
assert again == text
assert opt.locate(again)[2] == original
rejections = 0
for altered in [original.replace("spu_fm(", "spu_fa(", 1),
                text.replace("spu__xr_fm(", "spu__xr_fa(", 1),
                text[:start] + opt.region(original).replace("--test_budget", "unused") +
                  text[end:] + opt.BEGIN]:
    try:
        if altered.startswith("void "): opt.transform_body(altered)
        else: opt.locate(altered)
    except ValueError:
        rejections += 1
    else:
        raise AssertionError("mutation was accepted")
report = dict(source=str(opt.SOURCE), original_body_sha256=opt.ORIGINAL_SHA256,
              native_body_sha256=opt.sha256(native.encode()),
              source_sha256=hashlib.sha256(opt.SOURCE.read_bytes()).hexdigest(),
              rea_evidence_id=rea["evidence_id"], original_opcodes_match_rea=True,
              idempotent=True, corruption_rejections=rejections,
              zero_count="Production remains decrement-before-check; instrumented prefix variants insert a test-only exit after an iteration and flush cached registers. Zero is not treated as an empty loop.",
              artifact=artifact)
(OUT / "extraction.json").write_text(json.dumps(report, indent=2)+"\n")
print(json.dumps({"fixture":str(OUT),"identity_and_idempotence":"PASS","rejections":rejections}))
