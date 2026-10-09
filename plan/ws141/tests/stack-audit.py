#!/usr/bin/env python3
# zedBSD, Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Audits the ordinary WS141 runtime stack in the final AArch64 LTO image.

Usage: python3 plan/ws141/tests/stack-audit.py DISASSEMBLY OUTPUT_JSON

This is a source-context bound, not a general kernel stack verifier. Callback
sets, recursion limits, one IRQ and ioctl context are justified in the p007
software audit. Terminal hal_fatal diagnostics are excluded explicitly. The
checker rejects unknown stack adjustment, unclassified indirect calls and
unbounded graph cycles rather than assigning them a zero frame.
"""
import sys
import re, json
from pathlib import Path
source=Path(sys.argv[1]).read_text()
blocks=re.split(r'\n([0-9a-f]{16}) <([^>]+)>:\n',source)
functions={};addresses={}
for i in range(1,len(blocks),3):
 address=int(blocks[i],16);name=blocks[i+1];body=blocks[i+2]
 frame=0;unknown=[];direct=[];indirect=[]
 for line in body.splitlines():
  m=re.search(r'\bsub\s+sp, sp, #(0x[0-9a-f]+|[0-9]+)(?:, lsl #(0x[0-9a-f]+|[0-9]+))?',line)
  if m:frame+=int(m[1],0) << (int(m[2],0) if m[2] else 0)
  m=re.search(r'\[sp, #-(0x[0-9a-f]+|[0-9]+)\]!',line)
  if m:frame+=int(m[1],0)
  if re.search(r'\b(sub|add)\s+sp, sp, [xw]',line):unknown.append(line.strip())
  m=re.search(r'\b(bl|b)\s+0x([0-9a-f]+)\s+<([^>]+)>',line)
  if m:direct.append((int(m[2],16),m[3]))
  if re.search(r'\b(blr|br)\s+x',line):indirect.append(line.strip())
 functions[name]={'address':address,'frame':frame,'direct_raw':direct,'indirect':indirect,'unknown':unknown}
 addresses[address]=name
for name,f in functions.items():
 f['direct']=sorted({addresses[a] for a,label in f.pop('direct_raw') if a in addresses})
# Fixed production callback tables; additional unknown edges remain explicit.
routes=['device','queue','sync','query','memory','resource','input','layout','descriptor_pool','descriptor_sets','descriptor_update','target','pipeline','command_pool','command_batch','command_buffer','barrier','buffer_copy','transfer','record']
for name in ['bcm2711_vulkan_dispatch','execute_stream','worker_main']:
 if name not in functions:continue
 if name=='bcm2711_vulkan_dispatch':extra=['bcm2711_vulkan_'+r+'_dispatch' for r in routes]
 elif name=='execute_stream':extra=['bcm2711_vulkan_dispatch']
 else:extra=['execute_command','execute_marker','dispose_command','dispose_marker']
 functions[name]['direct']+= [x for x in extra if x in functions]
roots=['worker_main','runtime_command','runtime_submit']
seen=set();todo=list(roots)
while todo:
 name=todo.pop()
 if name in seen:continue
 seen.add(name);todo+=functions[name]['direct']
# Report every direct strongly connected component without pretending graph cycles have zero stack cost.
indices={};low={};stack=[];onstack=set();components=[]
def visit(n):
 indices[n]=len(indices);low[n]=indices[n];stack.append(n);onstack.add(n)
 for child in functions[n]['direct']:
  if child not in seen:continue
  if child not in indices:visit(child);low[n]=min(low[n],low[child])
  elif child in onstack:low[n]=min(low[n],indices[child])
 if low[n]==indices[n]:
  component=[]
  while True:
   child=stack.pop();onstack.remove(child);component.append(child)
   if child==n:break
  if len(component)>1 or n in functions[n]['direct']:components.append(sorted(component))
for n in sorted(seen):
 if n not in indices:visit(n)
report={'roots':roots,'reachable':len(seen),'cycles':components,'unknown_sp':{n:functions[n]['unknown'] for n in sorted(seen) if functions[n]['unknown']},'indirect':{n:functions[n]['indirect'] for n in sorted(seen) if functions[n]['indirect']},'largest_frames':sorted([(functions[n]['frame'],n) for n in seen],reverse=True)[:25]}
inventory=report



import json
from pathlib import Path
from functools import lru_cache
f=functions
# Fixed callback targets from actual private publication tables and typed record construction.
bindings={
 'bcm2711_vulkan_object_release':['release_root','release_sync','release_memory','release_resource','release_input','release_set_layout','release_pipeline_layout','release_pool','release_pool.2250','release_set','release_target','bcm2711_vulkan_pipeline_release','bcm2711_vulkan_command_release'],
 'bcm2711_vulkan_command_clear':['release_barrier','release_record','release_copy','release_transfer'],
 'publish_layout':['release_set_layout','release_pipeline_layout'],
 'walk_records':['prepare_event'],
 'handle_put':['fence_release'],
 'arm64_irq_handler':['irq_handler'],
 'irq_handler':['service_sources','service_timing','service_compositor'],
}
for n,children in bindings.items():
 for child in children:
  if child not in f:raise RuntimeError('Missing binding '+child)
 f[n]['direct']+=children
# Kernel IRQ on an already active driver kernel frame cannot take the EL0-return-only branch.
f['arm64_irq_handler']['direct']=[c for c in f['arm64_irq_handler']['direct'] if c!='kernel_user_return_handler']
# Ordinary runtime/refusal estimate excludes terminal kernel invariant-failure diagnostics.
for n in f:
 f[n]['direct']=[c for c in f[n]['direct'] if c!='hal_fatal']
# Workgroup layout is unreachable after graphics model/storage preflight.
for n in f:
 f[n]['direct']=[c for c in f[n]['direct'] if c not in ['i915_spirv_declare_shared','i915_spirv_shared_bytes','i915_spirv_shared_access']]
# Finite active frames justified by source, not arbitrary loop cutoffs.
# Object count includes one null-edge leaf after the eight-owner typed DAG.
limits={'execute_stream':5,'i915_spirv_operand_wide':9,'i915_spirv_type_size':10,'i915_spirv_io_map':10,'i915_spirv_type_has_int16':10,'i915_spirv_minor_determinant':4,'bcm2711_vulkan_object_release':9,'signal_send_process_info':2}
keys=tuple(limits)
active=set()
@lru_cache(None)
def bound(name,counts):
 context=(name,counts)
 if context in active:raise RuntimeError('Unbounded graph cycle '+str(context))
 counts=list(counts)
 leaf=False
 if name in limits:
  index=keys.index(name)
  counts[index]+=1
  if counts[index]>limits[name]:raise RuntimeError('Model failed to terminate at finite leaf')
  leaf=counts[index]==limits[name]
 counts=tuple(counts)
 active.add(context)
 maximum=(0,[])
 children=f[name]['direct']
 # Explicit boundary frames refuse excessive type depth, terminate scalars/null owners, or send SIGCHLD without process_continue.
 if leaf:
  if name=='execute_stream':children=[c for c in children if c!=name]
  elif name=='signal_send_process_info':children=[c for c in children if c!='process_continue']
  elif name=='bcm2711_vulkan_object_release':children=[]
  else:children=[c for c in children if c!=name]
 for child in children:
  candidate=bound(child,counts)
  if candidate[0]>maximum[0]:maximum=candidate
 active.remove(context)
 return f[name]['frame']+maximum[0],[name]+maximum[1]
roots=['worker_main','runtime_command','runtime_submit','arm64_irq_current','bcm2711_vulkan_pipeline_dispatch']
results={n:bound(n,(0,)*len(keys)) for n in roots}

# Whole reachable context retains every indirect instruction for classification.
seen=set();todo=list(roots)
while todo:
 name=todo.pop()
 if name in seen:continue
 seen.add(name);todo+=f[name]['direct']
switches={'bcm2711_shader_append','compile_program','i915_spirv_lower_atomic','i915_spirv_lower_compare','i915_spirv_lower_extended','i915_spirv_pass_body','i915_spirv_pass_declarations','signal_fields','walk_records'}
callbacks=set(bindings)|{'bcm2711_vulkan_dispatch','execute_stream','worker_main'}
absent_observers={'kern_malloc','kern_free'}
unknown={n:f[n]['indirect'] for n in seen if f[n]['indirect'] and n not in switches|callbacks|absent_observers}
assert not unknown, ('Unclassified indirect target',unknown)
assert not {n:f[n]['unknown'] for n in seen if f[n]['unknown']}, 'Unknown SP adjustment'
assert not any('heap_trace' in n or 'kern_heap_set_observer' in n for n in f), 'Observer configuration changed'
# The syscall branch leading to GPU COMMAND holds these fixed outer frames.
outer=['arm64_sync_lower','arm64_sync_handler','kernel_syscall_handler','file_ioctl','cdev_ioctl_file','gpu_ioctl','gpu_command_ioctl']
outer_bytes=sum(f[n]['frame'] for n in outer)
irq=results['arm64_irq_current'][0]
totals={'worker':f['kernel_thread_trampoline']['frame']+results['worker_main'][0]+irq,'synchronous_COMMAND':outer_bytes+results['runtime_command'][0]+irq,'asynchronous_SUBMIT':outer_bytes+results['runtime_submit'][0]+irq}
assert max(totals.values())<=16384, ('Runtime exceeds 16KiB stack',totals)
report={'kind':'ordinary configured runtime, one kernel IRQ; terminal invariant diagnostics excluded','limits':limits,'bindings':bindings,'roots':results,'outer_frames':{n:f[n]['frame'] for n in outer},'kernel_thread_prefix':f['kernel_thread_trampoline']['frame'],'whole_bytes':totals,'capacity':16384,'margin':16384-max(totals.values()),'reachable':len(seen),'indirect_classification':{'fixed_callbacks':sorted(callbacks),'LLVM_intrafunction_switches':sorted(switches),'NULL_heap_observers':sorted(absent_observers)},'unknown_indirect':unknown,'inventory':inventory}
Path(sys.argv[2]).write_text(json.dumps(report,indent=2)+'\n')
print('WS141 configured ordinary runtime/outer ioctl/one IRQ/finite recursion/indirect closure: PASS', totals, 'margin',report['margin'])
