#!/usr/bin/env python3
"""Build-time header generation from the single immutable catalog."""
import json,sys,re
from pathlib import Path
j=json.loads(Path(sys.argv[1]).read_text())
p=j['profiles']
if [v['name'] for v in p]!=['Common','SKT','KT','LGU']: raise ValueError('catalog order')
for v in p:
 if not re.fullmatch('[0-9a-f]{40}',v['id']): raise ValueError('config ID')
 if v['sha256'] and not re.fullmatch('[0-9a-f]{64}',v['sha256']): raise ValueError('MBN hash')
s='#pragma once\n#include <array>\n#include <cstdint>\nnamespace guacamole_catalog {\n'
s+='enum class Carrier:int { Common=0, SKT=1, KT=2, LGU=3 };\n'
s+='struct Profile {const char* name; const char* id; uint32_t size,version; const char* description; int index; const char* mbn_path; const char* sha256;};\n'
s+='inline constexpr std::array<Profile,4> kProfiles={{\n'
for v in p:
 s+=' {'+','.join([json.dumps(v['name']),json.dumps(v['id']),str(v['size']),str(v['version']),json.dumps(v['description']),str(v['index']),json.dumps(v['mbn_path']),json.dumps(v['sha256'])])+'},\n'
s+='}};\n'
for name,key in [('Stock','stock'),('Korea','korea')]:
 s+='inline constexpr uint32_t k'+name+'Size='+str(j[key]['size'])+';\n'
 s+='inline constexpr char k'+name+'Hash[]='+json.dumps(j[key]['sha256'])+';\n'
s+='}\n'
Path(sys.argv[2]).write_text(s)
