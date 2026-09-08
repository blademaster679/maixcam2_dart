#!/usr/bin/env python3
"""Inventory OS04A10 C sequences and Axera ELF64 arrays without executing them.

Partial tables are reported as partial; this does not reconstruct driver mode
selection or prove what an undocumented register bit does. Archive extraction
uses `ar p` into memory. No downloaded library is loaded or deployed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
from reverse_engineer import final_state


def summary(name, ops):
    state=final_state(ops)
    def word(a):
        return state[a]*256+state[a+1] if a in state and a+1 in state else None
    return dict(name=name,writes=len(ops),contains_reset=any(a==0x103 and v==1 for a,v,_ in ops),
                width=word(0x3808),height=word(0x380a),hts=word(0x380c),vts=word(0x380e),
                window=[word(a) for a in (0x3800,0x3802,0x3804,0x3806)],
                sampling_fields={f'0x{a:04x}':state.get(a) for a in range(0x3814,0x3818)},
                format_fields={f'0x{a:04x}':state.get(a) for a in range(0x3820,0x3823)})


def c_sequences(text):
    text=re.sub(r'/\*.*?\*/|//[^\n]*','',text,flags=re.S)
    result=[]
    for name,body in re.findall(r'\b(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\};',text,re.S):
        ops=[[int(a,16),int(v,16),0] for a,v in re.findall(r'\{\s*(0x[\da-fA-F]+)\s*,\s*(0x[\da-fA-F]+)',body)]
        if len(ops)>10: result.append(summary(name,ops))
    # Sophgo uses straight-line write_register calls in init functions.
    for name,body in re.findall(r'\bvoid\s+(\w+)\s*\([^;{}]*\)\s*\{(.*?)\n\}',text,re.S):
        ops=[[int(a,16),int(v,16),0] for a,v in re.findall(r'os04a10_write_register\(\s*ViPipe\s*,\s*(0x[\da-fA-F]+)\s*,\s*(0x[\da-fA-F]+)',body)]
        if len(ops)>10: result.append(summary(name,ops))
    return result


def elf_sequences(data):
    if data[:6]!=b'\x7fELF\x02\x01': raise ValueError('Expected little-endian ELF64')
    shoff=struct.unpack_from('<Q',data,40)[0]
    size,count=struct.unpack_from('<HH',data,58)
    sections=[struct.unpack_from('<IIQQQQIIQQ',data,shoff+i*size) for i in range(count)]
    result=[]
    for section in sections:
        if section[1]!=2: continue  # SHT_SYMTAB
        strings=sections[section[6]]
        names=data[strings[4]:strings[4]+strings[5]]
        for offset in range(section[4],section[4]+section[5],section[9]):
            name,info,other,index,value,length=struct.unpack_from('<IBBHQQ',data,offset)
            label=names[name:].split(b'\0',1)[0].decode(errors='replace')
            if info&15!=1 or not label.startswith('OS04A10_') or not 0<index<len(sections):continue
            s=sections[index]
            payload=data[s[4]+value-s[3]:s[4]+value-s[3]+length]
            if length%8: raise ValueError(f'Unexpected register array size: {label}')
            pairs=list(struct.iter_unpack('<II',payload))
            if any(a>65535 or v>255 for a,v in pairs):raise ValueError(f'Unexpected array layout: {label}')
            result.append(summary(label,[[a,v,0] for a,v in pairs]))
    return result


def inventory(path):
    path=Path(path); data=path.read_bytes()
    if path.suffix=='.a':
        obj=subprocess.run(['ar','p',str(path),'os04a10_reg.o'],check=True,capture_output=True).stdout
        modes=elf_sequences(obj)
    else: modes=c_sequences(data.decode())
    return dict(path=str(path.resolve()),sha256=hashlib.sha256(data).hexdigest(),
                sequences=modes,sequence_count=len(modes),
                note='Missing fields are unknown/inherited. Values do not establish binning semantics.')


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sources',type=Path,nargs='+');ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    results=[inventory(p) for p in args.sources]
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(json.dumps(results,indent=2)+'\n')
    for item in results:
        print(item['path'],item['sequence_count'])
        values={a:sorted({m['sampling_fields'][a] for m in item['sequences'] if m['sampling_fields'][a] is not None})
                for a in ['0x3814','0x3815','0x3816','0x3817']}
        print(values)
