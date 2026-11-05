"""Opt-in original dual-arena lifetime tracing; run after codegen/direct/leaf preparation.
Only adds scalar observation calls. No allocator branches, capacities, pointers or frees change.
"""
from pathlib import Path
import argparse,hashlib,json
p=argparse.ArgumentParser();p.add_argument('project',type=Path);p.add_argument('--backup',type=Path,required=True);a=p.parse_args();a.backup.mkdir(parents=True,exist_ok=True)
header='#include "src/native/stream_arena_trace.h"\n'
report={}
for filename,function,region in [('prototype2_recomp.154.cpp','sub_82CA2860','create'),('prototype2_recomp.68.cpp','sub_82CA1F50','normal'),('prototype2_recomp.35.cpp','sub_82CA2008','gpu'),('prototype2_recomp.1.cpp','sub_82CA22E0','free'),('prototype2_recomp.110.cpp','sub_82CA25C8','destroy')]:
 f=a.project/'generated/default'/filename;original=f.read_bytes();s=original.decode();b=s.index('DEFINE_REX_FUNC('+function+')');e=s.find('\nDEFINE_REX_FUNC(',b+1);e=len(s) if e<0 else e;before=s[b:e];after=before;edits=[]
 assert 'P2 arena history' not in before, 'Already instrumented '+function
 if region=='create':
  old='ctx.r3.u64 = r31.u64;\n\t// addi r1,r1,160';new='ctx.r3.u64 = r31.u64;\n\tif (P2ArenaTraceEnabled()) P2ArenaTraceRecord(1,r31.u32,REX_LOAD_U32(r31.u32+88),REX_LOAD_U32(r31.u32+92),REX_LOAD_U32(r31.u32+100),REX_LOAD_U32(r31.u32+104),0,0,nullptr); /* P2 arena history */\n\t// addi r1,r1,160';edits.append((old,new))
 elif region=='destroy':
  old='REX_FUNC_PROLOGUE();';edits.append((old,old+'\n\tif (P2ArenaTraceEnabled()) P2ArenaTraceRecord(5,ctx.r3.u32,0,0,0,0,0,0,nullptr); /* P2 arena history */'))
 elif region=='free':
  old='loc_82CA2374:\n';new=old+'\tif (P2ArenaTraceEnabled()) P2ArenaTraceRecord(4,r31.u32,r30.u32,REX_LOAD_U32(r31.u32+96),REX_LOAD_U32(r31.u32+108),REX_LOAD_U32(r31.u32+112),REX_LOAD_U32(r31.u32+116),REX_LOAD_U32(r31.u32+120),nullptr); /* P2 arena history */\n';edits.append((old,new))
 else:
  gpu=region=='gpu';kind=3 if gpu else 2;cursor=108 if gpu else 96;count=116 if gpu else 112
  edits.append(('REX_FUNC_PROLOGUE();','REX_FUNC_PROLOGUE();\n\tconst uint32_t p2_trace_alignment = ctx.r5.u32; /* P2 arena history */'))
  old='\t// addi r1,r1,'+('128' if gpu else '144')+'\n'
  new='\tif (P2ArenaTraceEnabled()) P2ArenaTraceRecord('+str(kind)+',r31.u32,'+('ctx.r3.u32' if gpu else 'r29.u32')+',r30.u32,p2_trace_alignment,REX_LOAD_U32(r31.u32+'+str(cursor)+'),REX_LOAD_U32(r31.u32+'+str(count)+'),REX_LOAD_U32(r31.u32+120),nullptr); /* P2 arena history */\n'+old
  # Only the success return. The identical failure epilogue remains untouched.
  assert after.count(old)==2
  after=after.replace(old,new,1)
  # Save exact positional edit separately for reversal.
  edits.append((old,new))
  label='loc_82CA2080:\n' if gpu else 'loc_82CA1FF8:\n'
  capture='\tif (P2ArenaTraceEnabled()) { /* P2 arena history */\n\t\tconst uint32_t state[10] = {'+','.join('REX_LOAD_U32(r31.u32+'+str(off)+')' for off in [88,92,96,112,100,104,108,116,120,124])+'};\n\t\tP2ArenaTraceRecord('+str(kind)+',r31.u32,0,r30.u32,p2_trace_alignment,state['+('6' if gpu else '2')+'],state['+('7' if gpu else '3')+'],state[8],state);\n\t}\n'
  edits.append((label,label+capture))
 for old,new in edits:
  if region in ('normal','gpu') and 'nullptr); /* P2 arena history */' in new:continue
  assert after.count(old)==1,(function,old);after=after.replace(old,new)
 check=after
 for old,new in reversed(edits):
  assert new in check,(function,new);check=check.replace(new,old,1)
 assert check==before,'Observation changed original statements: '+function
 (a.backup/filename).write_bytes(original);(a.backup/(function+'.original.inc')).write_text(before);(a.backup/(function+'.traced.inc')).write_text(after)
 f.write_text(header+s[:b]+after+s[e:],newline='')
 report[filename]={'original_sha256':hashlib.sha256(original).hexdigest(),'patched_sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'function':function,'reverse_exact':True,'edits':edits}
(a.backup/'patch-validation.json').write_text(json.dumps(report,indent=2));print('PASS reversible scalar observations in five original arena functions')
