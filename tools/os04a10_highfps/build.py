#!/usr/bin/env python3
"""Build an isolated Axera sensor extension and capture benchmark from local SDK.

Vendor sources remain in ignored .maixpy state; only this transformation and our
own code are maintained here. No shared SDK, global ABI header or board is edited.
"""
import argparse
import difflib
import json
import shlex
import shutil
import subprocess
from pathlib import Path
from profile import ProfileError, header, load, sha256

HERE = Path(__file__).resolve().parent
MSP_VERSION = '3.0.0_20250319114413'


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise RuntimeError(f'SDK source changed: expected {count} occurrences of {old[:80]!r}')
    return text.replace(old, new)


def patch_sources(sensor, profile, candidate_state=None):
    def edit(name, transform):
        p = sensor / name
        old = p.read_text()
        new = transform(old)
        p.write_text(new)
        return ''.join(difflib.unified_diff(old.splitlines(True), new.splitlines(True),
                                          fromfile='a/' + name, tofile='b/' + name))
    (sensor / 'hfr_generated.h').write_text(header(profile, candidate_state))
    shutil.copy2(HERE / 'hfr_sensor.inc', sensor)
    patches = []
    patches.append(edit('os04a10_settings.h', lambda s: replace(s,
        '    e_OS04A10_setting_sel_max',
        '    e_OS04A10_640x360_HFR,\n    e_OS04A10_setting_sel_max')))
    def sensor_patch(s):
        s = replace(s, '#include "os04a10_ae_ctrl.h"',
                    '#include "os04a10_ae_ctrl.h"\n#include "hfr_generated.h"')
        anchor = '    if (width == 2688 && height == 1520 && hdrmode == AX_SNS_LINEAR_MODE && nRawType == 10 && MasterSlaveSel == AX_SNS_MASTER) {'
        addition = '''    if (width == OS04A10_HFR_WIDTH && height == OS04A10_HFR_HEIGHT &&
        (OS04A10_HFR_WIDTH == 640 || IS_SNS_FPS_EQUAL(sns_mode->fFrameRate, OS04A10_HFR_FPS))) {
#if OS04A10_HFR_AVAILABLE
        if (hdrmode != AX_SNS_LINEAR_MODE || nRawType != 10 ||
            MasterSlaveSel != AX_SNS_MASTER || !isfinite(sns_mode->fFrameRate) ||
            sns_mode->fFrameRate < 1.0f || sns_mode->fFrameRate > OS04A10_HFR_FPS ||
            (sns_mode->nSettingIndex != 0 && sns_mode->nSettingIndex != e_OS04A10_640x360_HFR))
            return AX_SNS_ERR_NOT_SUPPORT;
        sns_setting_index = e_OS04A10_640x360_HFR;
        setting_fps = OS04A10_HFR_FPS;
#else
        SNS_ERR("OS04A10 360p mode unavailable: vendor binning sequence missing\\n");
        return AX_SNS_ERR_NOT_SUPPORT;
#endif
    } else '''
        s = replace(s, anchor, addition + anchor.strip())
        # Do not allow the optional setting-index override to bypass size/mode validation.
        s = replace(s, '    if (sns_mode->nSettingIndex > 0) {',
                    '    if (sns_mode->nSettingIndex == e_OS04A10_640x360_HFR && (width != OS04A10_HFR_WIDTH || height != OS04A10_HFR_HEIGHT))\n'
                    '        return AX_SNS_ERR_NOT_SUPPORT;\n    if (sns_mode->nSettingIndex > 0) {')
        # This SDK init callback returns void: stop before register programming if
        # identity/I2C initialization fails, and never mark initialization complete.
        s = replace(s, '    os04a10_sensor_i2c_init(nPipeId);',
                    '    if (os04a10_sensor_i2c_init(nPipeId) != AX_SNS_SUCCESS) return;')
        s = replace(s, '        SNS_ERR("can\'t find os04a10 sensor id.\\n");',
                    '        SNS_ERR("can\'t find os04a10 sensor id.\\n");\n        return;')
        s = replace(s, '    os04a10_write_settings(nPipeId);',
                    '    if (os04a10_write_settings(nPipeId) != AX_SNS_SUCCESS) return;')
        s = replace(s, '    os04a10_cfg_aec_param(nPipeId);',
                    '    if (os04a10_cfg_aec_param(nPipeId) != AX_SNS_SUCCESS) return;')
        return s
    patches.append(edit('os04a10.c', sensor_patch))
    def reg_patch(s):
        s = replace(s, '#include "os04a10_settings.h"',
                    '#include "os04a10_settings.h"\n#define OS04A10_HFR_DRIVER_TABLES\n#include "hfr_generated.h"')
        s = replace(s, '    switch (sns_obj->eImgMode) {', '''    switch (sns_obj->eImgMode) {
#if OS04A10_HFR_AVAILABLE
    case e_OS04A10_640x360_HFR:
        *setting = os04a10_hfr_final_state;
        *cnt = sizeof(os04a10_hfr_final_state) / sizeof(os04a10_hfr_final_state[0]);
        break;
#endif''')
        anchor = '    for (i = 0; i < reg_cnt; i++) {\n        os04a10_write_register(nPipeId, (setting + i)->addr, ((setting + i)->value));'
        s = replace(s, anchor, '''#if OS04A10_HFR_AVAILABLE
    if (setting == os04a10_hfr_final_state) {
        for (i = 0; i < sizeof(os04a10_hfr_sequence) / sizeof(os04a10_hfr_sequence[0]); ++i) {
            const os04a10_hfr_op *op = &os04a10_hfr_sequence[i];
            ret = os04a10_write_register(nPipeId, op->addr, op->value);
            if (ret != AX_SNS_SUCCESS) return ret;
            if (op->delay_us) usleep(op->delay_us);
        }
        return AX_SNS_SUCCESS;
    }
#endif
    for (i = 0; i < reg_cnt; i++) {
        ret = os04a10_write_register(nPipeId, (setting + i)->addr, ((setting + i)->value));
        if (ret != AX_SNS_SUCCESS) return ret;''')
        return s
    patches.append(edit('os04a10_reg.c', reg_patch))
    def ae_patch(s):
        s = replace(s, '#include "os04a10_settings.h"',
                    '#include "os04a10_settings.h"\n#include "hfr_sensor.inc"')
        # Do NOT change SNS_MAX_FRAME_RATE: it also sizes shared ABI structures.
        a = s.index('AX_S32 os04a10_set_fps(')
        b = s.index('AX_S32 os04a10_set_slow_fps(')
        part = s[a:b]
        gate = '    if(AXSNS_CAMPARE_FLOAT(AX_SNS_FPS_MIN, fFps) || AXSNS_CAMPARE_FLOAT(fFps, AX_SNS_FPS_MAX)) {'
        part = replace(part, gate, '''    if (sns_obj->eImgMode == e_OS04A10_640x360_HFR) {
        if (os04a10_hfr_check_fps(fFps, sns_obj->ae_ctrl_param.fTimePerLine) != AX_SNS_SUCCESS)
            return AX_SNS_ERR_NOT_SUPPORT;
    } else
''' + gate)
        part = replace(part, '    os04a10_set_vts(nPipeId, vts);',
                       '    if (os04a10_set_vts(nPipeId, vts) != AX_SNS_SUCCESS)\n        return AX_SNS_ERR_NOT_SUPPORT;')
        s = s[:a] + part + s[b:]
        # Disable slow-shutter gear paths for this mode, retaining existing ABI.
        for fn in ['os04a10_set_slow_fps', 'os04a10_get_slow_shutter_param']:
            marker = f'AX_S32 {fn}('
            if marker not in s:
                if fn == 'os04a10_get_slow_shutter_param':
                    continue
                raise RuntimeError(marker)
            start = s.index(marker)
            at = s.index('    SNS_CHECK_PTR_VALID(sns_obj);', start) + len('    SNS_CHECK_PTR_VALID(sns_obj);')
            s = s[:at] + '\n    if (sns_obj->eImgMode == e_OS04A10_640x360_HFR) return AX_SNS_ERR_NOT_SUPPORT;' + s[at:]
        s = replace(s, '    os04a10_get_params_from_setting(nPipeId);',
                    '    if (os04a10_get_params_from_setting(nPipeId) != AX_SNS_SUCCESS) return AX_SNS_ERR_NOT_MATCH;')
        # Vendor helper uses integer fractional dividers; profile SCLK was checked
        # with the documented fractional PLL formula before header generation.
        s = replace(s, '    sns_os04a10params[nPipeId].sclk = sclk;',
                    '#if OS04A10_HFR_AVAILABLE\n    SNS_STATE_OBJ *hfr_ctx = AX_NULL;\n    SENSOR_GET_CTX(nPipeId, hfr_ctx);\n    SNS_CHECK_PTR_VALID(hfr_ctx);\n    if (hfr_ctx->eImgMode == e_OS04A10_640x360_HFR) sclk = OS04A10_HFR_SCLK_HZ;\n#endif\n'
                    '    sns_os04a10params[nPipeId].sclk = sclk;')
        return s
    patches.append(edit('os04a10_ae_ctrl.c', ae_patch))
    return '\n'.join(patches)


def run(args, **kw):
    subprocess.run([str(a) for a in args], check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk', type=Path, default=Path.home()/'maix/MaixCDK')
    ap.add_argument('--profile', type=Path)
    ap.add_argument('--recipe', type=Path, help='Reproduce a source-pinned reverse-engineered crop recipe')
    ap.add_argument('--fullfov-probe', choices=['30','60','90'], help='Full native array timing control; does not enable binning or claim 360fps')
    ap.add_argument('--crop-probe', nargs=2, type=int, metavar=('HTS','VTS'),
                    help='Build an unverified crop candidate from baseline; not a vendor mode')
    ap.add_argument('--crop-full-width', action='store_true', help='Retain full horizontal array readout for comparison')
    ap.add_argument('--crop-sclk-mhz', type=int, choices=[72,108], default=72)
    ap.add_argument('--out', type=Path, default=Path('.maixpy/os04a10-build'))
    args = ap.parse_args()
    if args.fullfov_probe and (args.profile or args.recipe or args.crop_probe or args.crop_full_width or args.crop_sclk_mhz!=72):
        ap.error('--fullfov-probe cannot be combined with other mode options')
    recipe = None
    if args.recipe:
        if args.profile or args.crop_probe or args.crop_full_width or args.crop_sclk_mhz!=72:
            ap.error('--recipe cannot be combined with mode overrides')
        recipe=json.loads(args.recipe.read_text())
        if any(recipe.get(k)!=v for k,v in {'kind':'reverse_engineered_crop_recipe',
            'sensor':'OS04A10','width':640,'height':360,'full_width':True}.items()):
            ap.error('Unsupported crop recipe')
        if any(type(recipe.get(k)) is not int for k in ['hts','vts','sclk_mhz']):
            ap.error('Recipe timing fields must be integers')
        args.crop_probe=[recipe['hts'],recipe['vts']]
        args.crop_full_width=True
        args.crop_sclk_mhz=recipe['sclk_mhz']
    if args.profile and args.crop_probe:
        ap.error('Choose vendor profile or crop probe')
    if args.crop_full_width and not args.crop_probe:
        ap.error('--crop-full-width requires --crop-probe')
    if args.crop_sclk_mhz != 72 and not args.crop_probe:
        ap.error('--crop-sclk-mhz requires --crop-probe')
    profile = load(args.profile)[0] if args.profile else None
    candidate_state = None
    out = args.out.resolve()
    if out.exists():
        raise SystemExit('Output exists; use a new --out directory to preserve artifacts')
    sdk = args.sdk.resolve()
    msp = sdk / f'dl/extracted/maixcam2_msp_srcs/maixcam2_msp_arm64_glibc_v{MSP_VERSION}'
    src = msp/'component/isp_proton/sensor'
    if args.fullfov_probe:
        from fullfov import native_candidate
        profile,candidate_state = native_candidate(src/'ov_os04a10/os04a10_settings.h',int(args.fullfov_probe))
    if recipe and sha256(src/'ov_os04a10/os04a10_settings.h')!=recipe.get('settings_sha256'):
        ap.error('Recipe source hash does not match this SDK')
    if args.crop_probe:
        from reverse_engineer import candidate
        profile,candidate_state = candidate(src/'ov_os04a10/os04a10_settings.h', *args.crop_probe,
                                          full_width=args.crop_full_width, sclk_mhz=args.crop_sclk_mhz)
        if recipe and recipe.get('nominal_fps') != profile['fps']:
            ap.error('Recipe nominal_fps disagrees with timing parameters')
    out.mkdir(parents=True)
    for part in ['ov_os04a10', 'common', 'i2c', 'mipi_switch', 'include']:
        shutil.copytree(src/part, out/'sensor'/part)
    sensor = out/'sensor/ov_os04a10'
    source_hashes = {p.name: sha256(p) for p in (src/'ov_os04a10').glob('*.[ch]')}
    (out/'driver.patch').write_text(patch_sources(sensor, profile, candidate_state))
    (out/'sensor/include/ax_module_version.h').write_text('#define AXERA_MODULE_VERSION "os04a10-hfr-development"\n')
    compiler = next((sdk/'dl/extracted/toolchains/maixcam2').glob('*/bin/aarch64-none-linux-gnu-gcc'))
    inc = [msp/'out/arm64_glibc/include', out/'sensor/include', out/'sensor/common/include',
           out/'sensor/i2c', out/'sensor/mipi_switch', sensor/'params_file/ax620e']
    sources = sorted(sensor.glob('*.c'))
    sources = [p for p in sources if p.name != 'os04a10_qs.c']
    for sub in ['common/src', 'i2c', 'mipi_switch']:
        sources += sorted((out/'sensor'/sub).glob('*.c'))
    flags = ['-shared', '-fPIC', '-O2', '-Wall', '-Wno-unused-function', '-Wno-unused-variable',
             '-DLINUX', '-DUSE_DEFAULT_PARAM', '-fvisibility=hidden']
    cmd = [compiler, *flags, *['-I'+str(p) for p in inc], *sources,
           '-Wl,-soname,libsns_os04a10.so', '-lpthread', '-lm', '-o', out/'libsns_os04a10.so']
    run(cmd, stdout=(out/'build.log').open('w'), stderr=subprocess.STDOUT)
    manifest = {'msp_version': MSP_VERSION, 'source_sha256': source_hashes,
                'profile': profile, 'hfr_available': profile is not None,
                'library_sha256': sha256(out/'libsns_os04a10.so'),
                'build_command': [str(x) for x in cmd]}
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(out)
    print('Mode:', 'baseline only' if profile is None else f'{profile["status"]}, {profile["fps"]:.6f}fps')


if __name__ == '__main__':
    try:
        main()
    except ProfileError as exc:
        raise SystemExit(str(exc))
