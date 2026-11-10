import hashlib
import struct
import unittest
import controller_ota_migrate as migration

def table(entries):
    data = b''.join(struct.pack('<HBBII16sI', 0x50aa, kind, sub, off, size, name.encode(), flags)
                    for name, (kind, sub, off, size, flags) in entries.items())
    return (data + b'\xeb\xeb' + b'\xff'*14 + hashlib.md5(data).digest()).ljust(4096,b'\xff')

class MigrationTests(unittest.TestCase):
    def test_partition_checksums_and_exact_layouts(self):
        self.assertEqual(migration.partitions(table(migration.LEGACY)), migration.LEGACY)
        self.assertEqual(migration.partitions(table(migration.OTA)), migration.OTA)
        bad = bytearray(table(migration.OTA)); bad[8] ^= 1
        with self.assertRaises(ValueError): migration.partitions(bad)

    def test_storage_and_nvs_addresses_are_identical(self):
        for name in ('nvs','phy_init','storage'):
            self.assertEqual(migration.LEGACY[name],migration.OTA[name])
        self.assertEqual(migration.OTA['ota_1'][2]+migration.OTA['ota_1'][3],migration.OTA['storage'][2])

    def test_application_abi_chip_and_project(self):
        app = bytearray(352); app[0] = 0xe9; app[1] = 1; app[23] = 1
        struct.pack_into('<I',app,28,272)
        struct.pack_into('<H',app,12,9); struct.pack_into('<I',app,32,0xabcd5432)
        name = b'lamarzocco_knob_controller'; app[80:80+len(name)] = name
        app[288:304] = migration.ABI
        checksum=0xef
        for byte in app[32:304]: checksum ^= byte
        app[319]=checksum
        app[320:]=hashlib.sha256(app[:320]).digest()
        migration.validate_application(app)
        for off in (0,12,32,80,288,319,335):
            bad=app.copy(); bad[off]^=1
            with self.assertRaises(ValueError): migration.validate_application(bad)

    def test_initial_metadata_selects_slot_zero_without_claiming_health(self):
        data=migration.initial_otadata()
        self.assertEqual(len(data),8192)
        self.assertEqual(struct.unpack_from('<I',data,0)[0],1)
        self.assertEqual(struct.unpack_from('<I',data,24)[0],0xffffffff)
        self.assertEqual(data[4096:],b'\xff'*4096)

    def test_protected_regions_are_compared_including_journal(self):
        before=bytearray(migration.FLASH_BYTES); after=before.copy()
        after[0x30000]=1
        migration.validate_preserved(before,after)
        for off in (0x9000,0x29000,0x930000,0xc30000,0xd2ffff):
            bad=after.copy(); bad[off]=1
            with self.assertRaises(ValueError): migration.validate_preserved(before,bad)

if __name__ == '__main__':
    unittest.main()
