#!/usr/bin/env python3
"""Diagnostic grayscale view of observed Axera little-endian packed RAW10.

The packing interpretation was inferred from captured data; this is not an ISP
or a color/Bayer calibration. Requires numpy and Pillow on the host.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image


def unpack(data, width, height, stride):
    if stride%4 or width%2 or height%2 or stride<width:
        raise ValueError('Unsupported RAW geometry')
    row_bytes=stride*10//8
    if len(data)!=row_bytes*height:
        raise ValueError('RAW size disagrees with packed stride')
    a=np.frombuffer(data,dtype=np.uint8).reshape(height,stride//4,5).astype(np.uint16)
    b=np.empty((height,stride),dtype=np.uint16)
    b[:,0::4]=a[:,:,0]|((a[:,:,1]&3)<<8)
    b[:,1::4]=(a[:,:,1]>>2)|((a[:,:,2]&15)<<6)
    b[:,2::4]=(a[:,:,2]>>4)|((a[:,:,3]&63)<<4)
    b[:,3::4]=(a[:,:,3]>>6)|(a[:,:,4]<<2)
    return b[:,:width]


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('run',type=Path)
    args=ap.parse_args()
    meta=json.loads((args.run/'sample.json').read_text())
    if meta['format']!=133: raise SystemExit('Expected Axera packed RAW10 (133)')
    pixels=unpack((args.run/'sample.raw').read_bytes(),meta['width'],meta['height'],meta['stride'])
    gray=pixels.reshape(meta['height']//2,2,meta['width']//2,2).mean(axis=(1,3))
    low,high=np.percentile(gray,[1,99])
    gray=np.clip((gray-low)/max(1,high-low)*255,0,255).astype('uint8')
    Image.fromarray(gray).save(args.run/'preview-gray.png')
    print(args.run/'preview-gray.png')
