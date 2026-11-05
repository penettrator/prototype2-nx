"""Opt-in diagnostic preparation, AFTER normal codegen/leaf preparation.
Only instruments two failure branches; does not replace allocation or recovery.
"""
from pathlib import Path
import argparse,hashlib,json
p=argparse.ArgumentParser();p.add_argument("project",type=Path);p.add_argument("--backup",type=Path,required=True);a=p.parse_args()
a.backup.mkdir(parents=True,exist_ok=True)
changes={
 "prototype2_recomp.186.cpp":("sub_82C773E0",[
  ("REX_FUNC_PROLOGUE();", "REX_FUNC_PROLOGUE();\n\tconst uint32_t p2_resource_caller = uint32_t(ctx.lr);"),
  ("loc_82C774A0:\n", "loc_82C774A0:\n\tif (ctx.r3.u32 == 0) P2ResourceAllocationFailed(r28.u32, r29.u32, r31.u32, p2_resource_caller, reinterpret_cast<uintptr_t>(&ctx));\n")]),
 "prototype2_recomp.70.cpp":("sub_82C01EB8",[
  ("loc_82C02038:\n\t// li r5,0\n\tctx.r5.s64 = 0;\n\t// lwz r3,24(r30)\n\tctx.r3.u64 = REX_LOAD_U32(r30.u32 + 24);", "loc_82C02038:\n\t// li r5,0\n\tctx.r5.s64 = 0;\n\t// lwz r3,24(r30)\n\tctx.r3.u64 = REX_LOAD_U32(r30.u32 + 24);\n\tif (ctx.r3.u32 == 0) P2ResourceMissingBeforeLock(r30.u32, r28.u32, reinterpret_cast<uintptr_t>(&ctx));")])}
report={}
for filename,(function,edits) in changes.items():
 f=a.project/"generated/default"/filename; original=f.read_bytes();s=original.decode();
 if "#include \"src/native/resource_failure_probe.h\"" in s: continue
 start=s.index("DEFINE_REX_FUNC("+function+")");end=s.find("\nDEFINE_REX_FUNC(",start+1);end=len(s) if end<0 else end
 before=s[start:end];after=before
 for old,new in edits:
  assert after.count(old)==1,(filename,old);after=after.replace(old,new)
 check=after
 for old,new in reversed(edits):check=check.replace(new,old)
 assert check==before,"Instrumentation changes original semantics"
 out='#include "src/native/resource_failure_probe.h"\n'+s[:start]+after+s[end:]
 backup=a.backup/filename;assert not backup.exists(),"Refusing to replace backup";backup.write_bytes(original)
 f.write_text(out,newline="")
 report[filename]={"original_sha256":hashlib.sha256(original).hexdigest(),"patched_sha256":hashlib.sha256(f.read_bytes()).hexdigest(),"reverse_exact":True}
(a.backup/"patch-validation.json").write_text(json.dumps(report,indent=2));print(json.dumps(report))
