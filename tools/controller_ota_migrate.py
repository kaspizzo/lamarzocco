#!/usr/bin/env python3
"""One-time, explicit USB migration. Default: validate artifacts and print plan.

No chip erase. Backups contain private controller settings; keep them private.
"""
import argparse
import binascii
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

FLASH_BYTES = 0x1000000
LEGACY = {'nvs': (1, 2, 0x9000, 0x20000, 0), 'phy_init': (1, 1, 0x29000, 0x1000, 0),
          'factory': (0, 0, 0x30000, 0x900000, 0), 'storage': (1, 0x82, 0x930000, 0x400000, 0)}
OTA = {k: v for k, v in LEGACY.items() if k != 'factory'} | {
    'otadata': (1, 0, 0x2a000, 0x2000, 0), 'ota_0': (0, 0x10, 0x30000, 0x480000, 0),
    'ota_1': (0, 0x11, 0x4b0000, 0x480000, 0)}
ABI = b'LMCTRL-OTA-V1'.ljust(16, b'\0')

def validate_image(image):
    if len(image) < 24 or image[0] != 0xe9 or image[23] != 1 or not 1 <= image[1] <= 16:
        raise ValueError('Expected a hashed ESP image')
    cursor, checksum = 24, 0xef
    for _ in range(image[1]):
        if cursor+8 > len(image): raise ValueError('Truncated ESP segment header')
        size = struct.unpack_from('<I',image,cursor+4)[0]
        cursor += 8
        if cursor+size > len(image): raise ValueError('Truncated ESP segment data')
        for byte in image[cursor:cursor+size]: checksum ^= byte
        cursor += size
    check_at = cursor | 15
    if check_at+33 != len(image) or image[check_at] != checksum:
        raise ValueError('ESP image length/checksum mismatch')
    if hashlib.sha256(image[:check_at+1]).digest() != image[check_at+1:]:
        raise ValueError('ESP image SHA256 mismatch')

def partitions(raw):
    result = {}
    md5_seen = False
    for off in range(0, min(len(raw), 0xc00), 32):
        entry = raw[off:off+32]
        if entry[:2] == b'\xff\xff': break
        if entry[:2] == b'\xeb\xeb':
            if entry[16:] != hashlib.md5(raw[:off]).digest(): raise ValueError('Partition-table MD5 mismatch')
            md5_seen = True
            break
        magic, kind, sub, addr, size, name, flags = struct.unpack('<HBBII16sI', entry)
        if magic != 0x50aa: raise ValueError('Unknown partition entry')
        name = name.split(b'\0')[0].decode('ascii')
        if name in result: raise ValueError('Duplicate partition name')
        result[name] = kind, sub, addr, size, flags
    if not md5_seen: raise ValueError('Partition table has no verified MD5 entry')
    return result

def validate_application(app):
    validate_image(app)
    if not 304 <= len(app) <= 0x480000: raise ValueError('Application size does not fit OTA slot')
    if app[0] != 0xe9 or struct.unpack_from('<H', app, 12)[0] != 9 or app[23] != 1:
        raise ValueError('Application target is not ESP32-S3')
    if struct.unpack_from('<I', app, 32)[0] != 0xabcd5432 or app[80:112].split(b'\0')[0] != b'lamarzocco_knob_controller':
        raise ValueError('Wrong controller application')
    if app[288:304] != ABI: raise ValueError('Missing OTA layout ABI; do not install an old non-OTA application')

def initial_otadata():
    # Same CRC convention as ESP-IDF 5.5.4 app_update/otatool.py.
    # UNDEFINED boots ota_0 without falsely claiming a successful startup.
    seq = struct.pack('<I', 1)
    entry = seq + b'\xff' * 24 + struct.pack('<I', binascii.crc32(seq, 0xffffffff) & 0xffffffff)
    return entry + b'\xff' * (0x2000 - len(entry))

def validate_preserved(before, after):
    if len(before) != FLASH_BYTES or len(after) != FLASH_BYTES: raise ValueError('Incomplete flash backup/readback')
    for name in ('nvs', 'phy_init', 'storage'):
        _, _, off, size, _ = LEGACY[name]
        if before[off:off+size] != after[off:off+size]: raise ValueError(f'{name} preservation check failed; keep controller in ROM mode')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--port')
    parser.add_argument('--expected-mac', required=True)
    parser.add_argument('--backup-dir', type=Path, required=True)
    parser.add_argument('--execute', action='store_true', help='Actually back up and migrate the attached controller')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    files = {'0x0': build/'bootloader/bootloader.bin', '0x8000': build/'partition_table/partition-table.bin',
             '0x30000': build/'lamarzocco_knob_controller.bin'}
    data = {address: path.read_bytes() for address, path in files.items()}
    validate_image(data['0x0'])
    validate_application(data['0x30000'])
    if partitions(data['0x8000']) != OTA: raise ValueError('Unexpected target partition map')
    if len(data['0x0']) > 0x8000: raise ValueError('Bootloader overlaps partition table')
    config = json.loads((build/'config/sdkconfig.json').read_text())
    if not config.get('BOOTLOADER_APP_ROLLBACK_ENABLE'): raise ValueError('Build must enable bootloader rollback')
    manifest = {'execute': args.execute, 'expected_mac': args.expected_mac.lower(),
                'images': {k: {'path': str(files[k]), 'sha256': hashlib.sha256(v).hexdigest()} for k, v in data.items()},
                'preserved': ['nvs at 0x9000', 'phy_init at 0x29000', 'storage at 0x930000 (including debug journal)']}
    print(json.dumps(manifest, indent=2))
    if not args.execute: return
    if not args.port: raise ValueError('--port is required for --execute')
    if not re.fullmatch(r'(?:[0-9a-f]{2}:){5}[0-9a-f]{2}', args.expected_mac.lower()): raise ValueError('Invalid expected MAC')
    os.umask(0o077)
    args.backup_dir.mkdir(mode=0o700, parents=True, exist_ok=False)
    def command(arguments, after='no_reset', before='default_reset'):
        result = subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', args.port,
                                 '--baud', '921600', '--before', before, '--after', after, *arguments],
                                capture_output=True, text=True)
        with (args.backup_dir/'esptool.log').open('a') as log: log.write(result.stdout + result.stderr)
        result.check_returncode()
        return result.stdout + result.stderr
    backup = args.backup_dir/'flash-before.bin'
    observed = command(['read_flash', '0', hex(FLASH_BYTES), str(backup)])
    if not re.search(r'\bmac:\s*' + re.escape(args.expected_mac.lower()) + r'\b', observed.lower()):
        raise ValueError('Attached device MAC did not match; no writes performed')
    before = backup.read_bytes()
    if len(before) != FLASH_BYTES or partitions(before[0x8000:0x9000]) != LEGACY:
        raise ValueError('Only the known legacy factory layout can be migrated; no writes performed')
    # Freeze checked artifacts in the private backup directory before writing.
    frozen = {}
    for address, contents in data.items():
        target = args.backup_dir/(address + '.bin')
        target.write_bytes(contents)
        frozen[address] = target
    ota = args.backup_dir/'otadata-initial.bin'; ota.write_bytes(initial_otadata())
    write_args = ['write_flash', '--flash_mode', 'keep', '--flash_freq', 'keep', '--flash_size', 'keep']
    for address, path in sorted((frozen | {'0x2a000': ota}).items(), key=lambda pair: int(pair[0],16)):
        write_args += [address, str(path)]
    manifest['backup_sha256'] = hashlib.sha256(before).hexdigest()
    (args.backup_dir/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    command(write_args)
    after_path = args.backup_dir/'flash-after.bin'
    command(['read_flash', '0', hex(FLASH_BYTES), str(after_path)])
    validate_preserved(before, after_path.read_bytes())
    command(['chip_id'], after='hard_reset', before='no_reset')
    print('Migration written and protected regions verified. Now verify portal, presets, logo, logs and healthy boot.')

if __name__ == '__main__':
    main()
