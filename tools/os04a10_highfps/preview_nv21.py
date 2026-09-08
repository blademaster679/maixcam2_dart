#!/usr/bin/env python3
"""Diagnostic BT.601 limited-range rendering of a contiguous VIN NV21 sample."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image


def decode(data, width, height, stride):
    if width%2 or height%2 or stride<width or len(data)!=stride*height*3//2:
        raise ValueError('Unsupported NV21 dimensions/stride/frame size')
    source=np.frombuffer(data,dtype=np.uint8)
    y=source[:stride*height].reshape(height,stride)[:,:width].astype(np.int32)-16
    chroma=source[stride*height:].reshape(height//2,stride)[:,:width].astype(np.int32)
    v=(chroma[:,0::2]-128).repeat(2,axis=0).repeat(2,axis=1)
    u=(chroma[:,1::2]-128).repeat(2,axis=0).repeat(2,axis=1)
    rgb=np.stack([(298*y+409*v+128)>>8,
                  (298*y-100*u-208*v+128)>>8,
                  (298*y+516*u+128)>>8],axis=2)
    return np.clip(rgb,0,255).astype(np.uint8)


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('run',type=Path)
    args=ap.parse_args()
    meta=json.loads((args.run/'sample.json').read_text())
    if meta['format']!=4: raise SystemExit('Expected Axera NV21 (4)')
    image=decode((args.run/'sample.nv21').read_bytes(),meta['width'],meta['height'],meta['stride'])
    Image.fromarray(image).save(args.run/'preview-nv21.png')
    print(args.run/'preview-nv21.png')
