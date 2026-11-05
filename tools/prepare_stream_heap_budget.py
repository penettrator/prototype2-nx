"""Keep the two city-heap wrappers visible when P2_DIRECT_CALLS bypasses weak overrides.
Run after codegen/direct-call preparation. Original game bodies remain unchanged.
"""
from pathlib import Path
import argparse,hashlib,json
p=argparse.ArgumentParser();p.add_argument('project',type=Path);p.add_argument('--backup',type=Path,required=True);a=p.parse_args();a.backup.mkdir(parents=True,exist_ok=True)
r={}
for filename,caller,target in [('prototype2_recomp.4.cpp','sub_82397BD0','sub_822F0C20'),('prototype2_recomp.100.cpp','sub_822F0C20','sub_82CA29E0')]:
 f=a.project/'generated/default'/filename;original=f.read_bytes();s=original.decode();b=s.index('DEFINE_REX_FUNC('+caller+')');e=s.find('\nDEFINE_REX_FUNC(',b+1);e=len(s) if e<0 else e
 before=s[b:e];old='P2_DIRECT_CALL('+target+')(ctx, base);';new=target+'(ctx, base); /* P2 stream-budget override */'
 if new in before:continue
 assert before.count(old)==1,(filename,caller,target)
 after=before.replace(old,new);assert after.replace(new,old)==before
 (a.backup/filename).write_bytes(original);f.write_text(s[:b]+after+s[e:],newline='')
 r[filename]={'before':hashlib.sha256(original).hexdigest(),'after':hashlib.sha256(f.read_bytes()).hexdigest(),'reverse_exact':True}
(a.backup/'patch-validation.json').write_text(json.dumps(r,indent=2));print('PASS two targeted direct-call wrapper routes',json.dumps(r))
