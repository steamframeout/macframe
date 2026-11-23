#!/usr/bin/env python3
"""Check that our rtw89 modules import every stock-kernel symbol with the CRC the stock kernel exports.
usage: check_modversions.py Module.symvers MODULE.ko..."""
import sys,struct,subprocess,os,tempfile
tmp=tempfile.mktemp()
OBJCOPY=os.environ.get("READELF","aarch64-linux-gnu-readelf").replace("readelf","objcopy")
symvers={}
for l in open(sys.argv[1]):
    c,s,mod,*_=l.split('\t'); symvers[s]=(int(c,16),mod)
tot=bad=0
for ko in sys.argv[2:]:
    subprocess.run([OBJCOPY,'-O','binary','--only-section=__versions',ko,tmp],check=True)
    b=open(tmp,'rb').read()
    for i in range(0,len(b),64):
        crc=struct.unpack('<Q',b[i:i+8])[0]; name=b[i+8:i+64].split(b'\0')[0].decode()
        c,mod=symvers.get(name,(None,''))
        if 'rtw89' in mod: continue   # provided by our own rtw89 build
        tot+=1
        if c!=crc: bad+=1; print('MISMATCH',os.path.basename(ko),name,mod)
print("modversions: external imports",tot,"mismatches",bad)
sys.exit(1 if bad else 0)
