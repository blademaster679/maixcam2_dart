#!/usr/bin/env python3
"""Compile an independent benchmark using this project's existing CDK build flags."""
import argparse
import json
import shlex
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, default=Path('build'))
    ap.add_argument('--driver-build', type=Path, default=Path('.maixpy/os04a10-build'))
    args = ap.parse_args()
    build = args.build.resolve()
    out = args.driver_build.resolve()
    flags = {}
    for line in (build/'main/CMakeFiles/main.dir/flags.make').read_text().splitlines():
        if ' = ' in line:
            k, v = line.split(' = ', 1); flags[k] = shlex.split(v)
    original = shlex.split((build/'CMakeFiles/dart_green_detect.dir/link.txt').read_text())
    compiler = original[0]
    # Own copy: force AI-ISP off during SYS pool setup without persisting config.
    incs = flags['CXX_INCLUDES']
    middleware = next(Path(x[2:])/'ax_middleware.hpp' for x in incs
                      if x.startswith('-I') and (Path(x[2:])/'ax_middleware.hpp').is_file())
    text = middleware.read_text()
    expr = 'app::get_sys_config_kv("npu", "ai_isp", "1") == "1" ? AX_TRUE : AX_FALSE'
    if expr not in text:
        raise RuntimeError('SDK SYS setup changed')
    (out/'ax_middleware.hpp').write_text(text.replace(expr, 'AX_FALSE'))
    shutil.copy2(HERE/'capture.cpp', out/'capture_source.cpp')
    cmd = [compiler, '-I'+str(out), '-I'+str(out/'sensor/ov_os04a10'),
           *flags['CXX_DEFINES'], *incs, *flags['CXX_FLAGS'], '-c', str(out/'capture_source.cpp'),
           '-o', str(out/'capture.o')]
    subprocess.run(cmd, check=True)
    # Reuse SDK dependency ordering but never link the project's application code.
    link = [x for x in original if x not in ['CMakeFiles/dart_green_detect.dir/exe_src.c.o', 'main/libmain.a']]
    link.insert(1, '-Wl,--as-needed')
    idx = link.index('-o'); link[idx+1] = str(out/'capture')
    link.insert(idx, str(out/'capture.o'))
    subprocess.run(link, cwd=build, check=True)
    (out/'capture_build.json').write_text(json.dumps({'compile':cmd,'link':link},indent=2))
    print(out/'capture')


if __name__ == '__main__':
    main()
