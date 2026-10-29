#!/usr/bin/env python3
"""extract_roms.py SOURCE...: copy the ROM files the translator needs into rom/ from MAME zips (dmnfrnt.zip,
pgm.zip) or folders, then write the big-endian 68000 images (m68k_split.py) and the decrypted ARM ROM
(decrypt_arm.py)."""
import os, sys, zipfile, subprocess
P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NEED = ['pgm_p02s.u20', 'v105_16m.u5', 'v105_32m.u26']
os.makedirs(os.path.join(P, 'rom'), exist_ok=True)
found = set()
for src in sys.argv[1:]:
    if os.path.isdir(src):
        for n in NEED:
            if n not in found and os.path.isfile(os.path.join(src, n)):
                open(os.path.join(P, 'rom', n), 'wb').write(open(os.path.join(src, n), 'rb').read()); found.add(n)
    elif zipfile.is_zipfile(src):
        with zipfile.ZipFile(src) as z:
            for i in z.infolist():
                n = os.path.basename(i.filename)
                if n in NEED and n not in found:
                    open(os.path.join(P, 'rom', n), 'wb').write(z.read(i)); found.add(n)
missing = [n for n in NEED if n not in found]
if missing:
    sys.exit('missing: ' + ' '.join(missing))
for t in ('m68k_split.py', 'decrypt_arm.py'):
    subprocess.check_call([sys.executable, os.path.join(P, 'tools', t)])
