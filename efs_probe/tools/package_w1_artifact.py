#!/usr/bin/env python3
"""Build-time validation/embedding, not an XML generator. Exact reviewed bytes."""
import hashlib
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET
def validate(data, catalog=None):
    if catalog is None:
        catalog=json.loads((Path(__file__).resolve().parents[2]/'carrier/catalog.json').read_text())
    SOURCE=catalog['stock']['sha256']
    TARGET=catalog['korea']['sha256']
    if len(data) != catalog['korea']['size'] or hashlib.sha256(data).hexdigest() != TARGET:
        raise ValueError('reviewed target size/hash mismatch')
    root = ET.fromstring(data)
    records = root.findall('.//SelRecord')
    if len(records) != 131:
        raise ValueError('record count')
    blocks = []
    for name, plmn, iin in [('SKT','450-05','898205'),('KT','450-08','898230'),('LGU','450-06','898206')]:
        index=str(next(p['index'] for p in catalog['profiles'] if p['name']==name))
        nodes = [x for x in root.findall('./if')
                 if any(r.get('carrier_name') == name for r in x.findall('.//SelRecord'))]
        if len(nodes) != 1:
            raise ValueError('Korea duplicate/missing')
        node = nodes[0]
        record = node.find('./then/SelRecord')
        if record.attrib != dict(carrier_name=name, country_code='410', iin='iin',
                                 imsi_3gpp='3gpp_imsi', mcfg_carrier_index=index,
                                 volte='true', vowifi='false'):
            raise ValueError('Korea attributes')
        if node.findtext('./any_of/iin_in') != iin or node.findtext('./any_of/imsi_3gpp_plmn_in') != plmn:
            raise ValueError('Korea predicate')
        candidates = [m.group(0) for m in re.finditer(rb'  <if>\n(?:(?!  </if>).)*  </if>\n', data, re.S)
                      if b'carrier_name="' + name.encode() + b'"' in m.group(0)]
        if len(candidates) != 1:
            raise ValueError('exact insertion boundary')
        blocks.append(candidates[0])
    insertion = b''.join(blocks)
    if data.count(insertion) != 1:
        raise ValueError('ordering/contiguity')
    original = data.replace(insertion, b'', 1)
    if len(original) != catalog['stock']['size'] or hashlib.sha256(original).hexdigest() != SOURCE:
        raise ValueError('original bytes not preserved')
    original_root = ET.fromstring(original)
    if len(original_root.findall('.//SelRecord')) != 128:
        raise ValueError('original records')
    common = b'  <if>\n    <tristate_reset_all return="true" />\n    <then>\n       <SelRecord carrier_name="Common"'
    if insertion + common not in data:
        raise ValueError('not immediately before unchanged Common')
    return original

def main():
    data = Path(sys.argv[1]).read_bytes()
    original = validate(data,json.loads(Path(sys.argv[2]).read_text()))
    text = '#pragma once\n#include <vector>\n#include <cstdint>\nnamespace guacamole_efs::w1 {\n'
    for name, blob in [('PackagedProposal', data), ('PackagedOriginal', original)]:
        if b')W1XML"' in blob:
            raise ValueError('literal delimiter')
        text += 'inline std::vector<uint8_t> ' + name + '() {\n'
        text += '  static constexpr char data[]=R"W1XML(' + blob.decode('utf-8') + ')W1XML";\n'
        text += '  return {data,data+sizeof(data)-1};\n}\n'
    Path(sys.argv[3]).write_text(text+'}\n', encoding='utf-8')
if __name__ == '__main__':
    main()
