#!/usr/bin/env python3
"""Decode resolution changes and inject the metadata mismatch seen in crash logs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plugin-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expect-abort', action='store_true',
                        help='verify the unpatched decoder reproduces the known SIGABRT')
    args = parser.parse_args()
    os.umask(0o077)
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    plugin = args.plugin_dir.resolve()/'libgstlibav.so'
    if not plugin.is_file():
        raise SystemExit('Missing decoder plugin: '+str(plugin))
    source = Path(__file__).with_suffix('.c')
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs',
        'gstreamer-app-1.0', 'gstreamer-video-1.0'], text=True))
    helper = output/'decode-switch'
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', str(source),
                    '-o', str(helper), *flags], check=True)
    segments = []
    for name, color, size in [('red', 'red', '318x242'), ('blue', 'blue', '640x384')]:
        path = output/(name+'.h264')
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
            '-i', f'color=c={color}:s={size}:r=12', '-frames:v', '12', '-c:v', 'libx264',
            '-pix_fmt', 'yuv420p', '-threads', '1', '-preset', 'medium', '-x264-params',
            'bframes=2:keyint=12:repeat-headers=1', '-f', 'h264', str(path)], check=True)
        segments.append(path.read_bytes())
    stream = output/'resolution-switch.h264'
    stream.write_bytes(b''.join(segments)*12)
    report = {'plugin': str(plugin), 'plugin_sha256': hashlib.sha256(plugin.read_bytes()).hexdigest(),
        'expected_abort_control': args.expect_abort, 'cases': [], 'passed': False,
        'scope': 'deterministic fatal-condition injection plus frame verification; website trigger not reconstructed'}
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    cases = [('normal', None), ('mismatched-meta', 'padded')]
    if not args.expect_abort:
        cases.extend([('mismatched-height', 'height'), ('undersized-meta', 'undersized'),
                      ('incompatible-meta', 'incompatible')])
    for name, mode in cases:
        log = output/(name+'.log')
        env = dict(os.environ, GST_PLUGIN_PATH=str(plugin.parent),
                   GST_REGISTRY=str(output/(name+'.registry.bin')))
        expected_abort = mode is not None and args.expect_abort
        with log.open('x') as out:
            process = subprocess.run([str(helper), str(stream),
                *(['padded-abort-control' if expected_abort else mode] if mode else [])],
                stdout=out, stderr=subprocess.STDOUT, env=env, timeout=45)
        content = log.read_text()
        selected = 'Selected decoder plugin: '+str(plugin)
        assert selected in content, 'Requested decoder was not loaded: '+content[-1000:]
        if expected_abort:
            assert process.returncode == -6 and 'vmeta->width == GST_VIDEO_INFO_WIDTH' in content, content[-2000:]
            result = {'expected_sigabrt': True}
        else:
            assert process.returncode == 0, content[-2000:]
            result = json.loads(content.splitlines()[-1])
            if mode in ('undersized', 'incompatible'):
                assert result['rejected_layout'] and result['failed'] and result['frames'] < 288
            else:
                assert result['frames'] == 288 and result['red_frames'] == result['blue_frames'] == 144
                assert result['meta_frames'] == 288 and result['eos'] and not result['failed']
            assert not mode or result['padded_allocations'] > 0
        report['cases'].append({'name': name, 'exit_code': process.returncode, 'result': result})
        (output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    report['passed'] = True
    (output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
