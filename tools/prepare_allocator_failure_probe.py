"""Opt-in bridge for the game's three allocator-failure messages. After codegen/leaf preparation."""
from pathlib import Path
import argparse,json,hashlib
p=argparse.ArgumentParser();p.add_argument("project",type=Path);p.add_argument("--backup",type=Path,required=True);a=p.parse_args();a.backup.mkdir(parents=True,exist_ok=True)
f=a.project/"generated/default/prototype2_recomp.154.cpp";data=f.read_bytes();s=data.decode();tag="#include \"src/native/resource_failure_probe.h\"\n"
assert tag not in s,"Already instrumented"
b=s.index("DEFINE_REX_FUNC(sub_822E9270)");e=s.find("\nDEFINE_REX_FUNC(",b+1);e=len(s) if e<0 else e
before=s[b:e];anchor="REX_FUNC_PROLOGUE();"
block="""
	// Failure-only bridge: exactly the game's three allocation error formats.
	if (ctx.r9.u32 == 0x8200A2D0 || ctx.r9.u32 == 0x8200A388 || ctx.r9.u32 == 0x8200A4E8) {
		const unsigned p2_count = ctx.r9.u32 == 0x8200A2D0 ? 9 : ctx.r9.u32 == 0x8200A388 ? 13 : 12;
		uint32_t p2_args[13]{};
		p2_args[0] = ctx.r10.u32;
		for (unsigned p2_i = 1; p2_i < p2_count; ++p2_i)
			p2_args[p2_i] = REX_LOAD_U32(ctx.r1.u32 + 84 + 8 * (p2_i - 1));
		P2AllocatorFailureMessage(ctx.r9.u32, p2_args, p2_count, uint32_t(ctx.lr), reinterpret_cast<uintptr_t>(&ctx));
	}
"""
assert before.count(anchor)==1;after=before.replace(anchor,anchor+block);assert after.replace(anchor+block,anchor)==before
backup=a.backup/f.name;assert not backup.exists();backup.write_bytes(data);out=tag+s[:b]+after+s[e:];f.write_text(out,newline="")
(a.backup/"entry-block.inc").write_text(block)
(a.backup/"patch-validation.json").write_text(json.dumps({"original_sha256":hashlib.sha256(data).hexdigest(),"patched_sha256":hashlib.sha256(f.read_bytes()).hexdigest(),"reverse_exact":True},indent=2));print("PASS exact reversible allocator-message bridge")
