"""Offline evidence and bounds tests; fixtures are never sensor mode tables."""
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import fullfov
from reverse_engineer import BASE
from source_inventory import c_sequences, elf_sequences


class FullFovTests(unittest.TestCase):
    def test_native_controls_preserve_geometry_and_bound_payload(self):
        regs={0x0103:1,0x0100:0,0x0305:0x3c,0x0306:0,0x0307:0,0x0308:4,0x030a:1,
              0x4837:14,0x0325:0x90,0x3814:1,0x3815:1,0x3816:1,0x3817:1}
        for a,v in [(0x3800,0),(0x3802,0),(0x3804,2703),(0x3806,1535),
                    (0x3808,2688),(0x380a,1520),(0x380c,732),(0x380e,3248),
                    (0x3810,9),(0x3812,9)]:regs.update({a:v>>8,a+1:v&255})
        def table(name,values):
            return 'static camera_i2c_reg_array '+name+'[] = {\n'+''.join(
                f'{{0x{a:x},0x{v:x}}},\n' for a,v in values.items())+'};\n'
        source=table(BASE,regs)
        source+=table('OS04A10_2lane_2688x1520_10bit_Linear_60fps',{0x0325:0xd8})
        source+=table('OS04A10_4lane_2688x1520_10bit_DCG_VS_HDR_30fps',regs|{0x0305:0x5c})
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'settings.h';path.write_text(source)
            with self.assertRaises(ValueError):fullfov.native_candidate(path,30)
            with patch.object(fullfov,'SETTINGS_SHA256',hashlib.sha256(path.read_bytes()).hexdigest()):
                for rate in (30,60,90):
                    p,s=fullfov.native_candidate(path,rate)
                    for a in range(0x3800,0x3823):
                        if a not in (0x380e,0x380f):self.assertEqual(s.get(a),regs.get(a))
                    self.assertLessEqual(p['fps'],90)
                    self.assertLess(p['width']*p['height']*p['fps']*10,p['lane_mbps']*4e6)
                with self.assertRaises(ValueError):fullfov.native_candidate(path,360)

    def test_source_comments_and_partial_tables(self):
        text='static regval mode[] = {\n'+''.join(f'{{0x{a:x},0x1}},\n' for a in range(0x3808,0x3818))+'};\n'
        text+='/* static regval fake[] = {\n{0x3814,0x07}\n}; */'
        found=c_sequences(text)
        self.assertEqual(len(found),1)
        self.assertFalse(found[0]['contains_reset'])
        self.assertIsNone(found[0]['window'][0])
        self.assertEqual(found[0]['sampling_fields']['0x3814'],1)

    def test_elf_register_layout_and_rejection(self):
        # Minimal ELF64 section/symbol fixture containing two 32-bit pairs.
        data=bytearray(512);data[:6]=b'\x7fELF\x02\x01'
        struct.pack_into('<Q',data,40,64);struct.pack_into('<HH',data,58,64,4)
        names=b'\0OS04A10_fixture\0';data[320:320+len(names)]=names
        struct.pack_into('<IIQQQQIIQQ',data,128,0,3,0,0,320,len(names),0,0,1,0)
        struct.pack_into('<IIQQQQIIQQ',data,192,0,2,0,0,352,24,1,0,8,24)
        struct.pack_into('<IIQQQQIIQQ',data,256,0,1,0,0,400,16,0,0,4,0)
        struct.pack_into('<IBBHQQ',data,352,1,1,0,3,0,16)
        struct.pack_into('<IIII',data,400,0x3814,1,0x3815,1)
        self.assertEqual(elf_sequences(data)[0]['sampling_fields']['0x3814'],1)
        struct.pack_into('<I',data,404,256)
        with self.assertRaises(ValueError):elf_sequences(data)


if __name__=='__main__':unittest.main()
