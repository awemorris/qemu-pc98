#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
"""PC-98 floppy IPL boot tests."""

import subprocess

from qemu_test import QemuSystemTest


class PC98FddBootTest(QemuSystemTest):
    # Exit through isa-debug-exit with value 0x2a.  In particular, bytes
    # 510-511 remain zero: PC-98 firmware does not require the IBM PC 55AA
    # marker.
    IPL = bytes.fromhex('ba0105b82a00effaf4ebfc')
    DEBUG_EXIT_STATUS = (0x2a << 1) | 1

    def boot_floppy(self, name, size):
        image = self.scratch_file(name)
        with open(image, 'wb') as floppy:
            floppy.truncate(size)
            floppy.write(self.IPL)

        proc = subprocess.run(
            [self.qemu_bin,
             '-M', 'pc9821',
             '-accel', 'tcg',
             '-display', 'none',
             '-monitor', 'none',
             '-serial', 'none',
             '-boot', 'a',
             '-device', 'isa-debug-exit,iobase=0x501,iosize=0x2',
             '-drive', f'if=floppy,format=raw,readonly=on,file={image}'],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=10,
            check=False)

        self.assertEqual(proc.returncode, self.DEBUG_EXIT_STATUS,
                         'PC-98 IPL was not executed:\n' + proc.stdout)

    def test_1200k_ipl_without_ibmpc_signature(self):
        self.boot_floppy('pc98-1200k.img', 2 * 80 * 15 * 512)

    def test_1232k_native_ipl_without_ibmpc_signature(self):
        self.boot_floppy('pc98-1232k.img', 2 * 77 * 8 * 1024)

    def test_1440k_ipl_without_ibmpc_signature(self):
        self.boot_floppy('pc98-1440k.img', 2 * 80 * 18 * 512)


if __name__ == '__main__':
    QemuSystemTest.main()
