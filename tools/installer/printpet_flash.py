"""PrintPet offline package verifier and guarded sparse installer.

Default/check is offline. --install/--upgrade plus --confirm-write is required
for serial access. No operation writes or erases the vendor identity gap.
"""
from pathlib import Path
from datetime import datetime,timezone
import argparse,hashlib,importlib.metadata,json,re,struct,subprocess,sys

ROOT=Path(__file__).resolve().parent
FLASH_BYTES=0x800000
APP_OFFSET=0x370000
APP_SIZE=0x400000
VENDOR=(0x310000,0x370000)
SETTINGS=(0x9000,0x10000)
ADDRESSES={'bootloader':0,'partitions':0x8000,'application':APP_OFFSET,'resources':0x10000}
SIZES={'bootloader':0x8000,'partitions':0x1000,'application':APP_SIZE,'resources':0x300000}

def sha(data):return hashlib.sha256(data).hexdigest()
def need(ok,message):
    if not ok:raise RuntimeError(message)
def save(path,data):
    with path.open('x',encoding='utf-8') as f:json.dump(data,f,ensure_ascii=False,indent=2)
def overlaps(a,b):return a[0]<b[1] and b[0]<a[1]

def parse_table(data):
    rows={};raw=bytearray()
    for at in range(0,len(data)-31,32):
        row=data[at:at+32];magic=struct.unpack_from('<H',row)[0]
        if magic==0xebeb:
            need(row[16:]==hashlib.md5(raw).digest(),'Partition-table MD5 mismatch')
            return rows
        need(magic==0x50aa,'Invalid partition table')
        _,kind,sub,offset,length,label,flags=struct.unpack('<HBBII16sI',row)
        name=label.split(b'\0')[0].decode('ascii');need(name not in rows,'Duplicate partition name')
        need(length>0 and offset+length<=FLASH_BYTES,'Partition exceeds 8MiB')
        need(not any(overlaps((offset,offset+length),(r[2],r[2]+r[3])) for r in rows.values()),'Overlapping partitions')
        rows[name]=(kind,sub,offset,length,flags);raw.extend(row)
    raise RuntimeError('Partition-table MD5 is absent')

def validate(folder=ROOT):
    folder=folder.resolve();m=json.loads((folder/'manifest.json').read_text(encoding='utf-8'))
    need(m['format']=='printpet-sparse-v1' and m['flashBytes']==FLASH_BYTES,'Unsupported package')
    need((m['product'],m['version']) in [('Core','1.5.2'),('Spirit','2.0.1')],'Wrong product/version')
    roles={};ranges=[]
    for part in m['parts']:
        role=part['role'];need(role in ADDRESSES and role not in roles,'Unknown or duplicate component')
        path=(folder/part['file']).resolve();need(path.is_relative_to(folder),'Component path escapes package')
        data=path.read_bytes();offset=part['offset']
        need(offset==ADDRESSES[role],f'Incorrect {role} address; never put the application at 0x0')
        need(0<len(data)<=SIZES[role] and len(data)==part['bytes'] and sha(data)==part['sha256'],'Component size/SHA mismatch')
        erased=(offset//0x1000*0x1000,(offset+len(data)+0xfff)//0x1000*0x1000)
        need(not overlaps(erased,VENDOR) and not overlaps(erased,SETTINGS),'Write/erase range touches protected storage')
        need(not any(overlaps(erased,r) for r in ranges),'Component erase ranges overlap')
        ranges.append(erased)
        if role in ['bootloader','application']:
            need(data[0]==0xe9 and struct.unpack_from('<H',data,12)[0]==5,'Not an ESP32-C3 image')
        if role=='application':
            at=data[:1024].find(struct.pack('<I',0xabcd5432));need(at>=0,'Application descriptor missing')
            version=data[at+16:at+48].split(b'\0')[0].decode('ascii')
            need(version==m['version'],'Image version differs from package')
        roles[role]=(path,data,part)
    need(set(roles)==({'bootloader','partitions','application','resources'} if m['product']=='Spirit' else {'bootloader','partitions','application'}),'Incomplete installation/recovery package')
    table=parse_table(roles['partitions'][1])
    need(table['factory'][2:4]==(APP_OFFSET,APP_SIZE),'Wrong application partition')
    need(table['spiffs'][2:4]==(0x10000,0x300000),'Wrong resource partition')
    need(table['nvs'][2:4]==(0x9000,0x6000) and table['phy_init'][2:4]==(0xf000,0x1000),'Unexpected settings layout')
    need(table['cardid'][2:4]==(0x356000,0x4000) and table['cardid'][4]&2,'Identity partition is not protected')
    return m,roles

def command(run,name,args,reader=False,before='no_reset',after='no_reset'):
    log=run/(name+'.log');need(not log.exists(),'Preserve prior operation: '+name)
    base=[sys.executable,str(ROOT/'esptool_window1.py')] if reader else [sys.executable,'-m','esptool']
    cmd=base+['--chip','esp32c3','--port',PORT,'--baud','115200','--before',before,'--after',after,*args]
    with log.open('xb') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,timeout=180)
    need(r.returncode==0,f'{name} failed. STOP; do not retry a write or boot automatically. See the private log.')
    print('PASS '+name,flush=True);return log.read_text(encoding='utf-8',errors='replace')

def read_range(run,name,offset,length):
    target=run/name;need(not target.exists(),'Readback target already exists')
    with target.open('xb') as f:
        for i,at in enumerate(range(0,length,0x40000)):
            size=min(0x40000,length-at);p=run/f'{name}-{i:02}.bin'
            command(run,f'read-{name}-{i:02}',['read_flash',hex(offset+at),hex(size),str(p)],reader=True)
            data=p.read_bytes();need(len(data)==size,'Truncated readback');f.write(data)
            print(json.dumps({'read':name,'bytes':at+size,'total':length}),flush=True)
    return target.read_bytes()

def flash(m,parts,args):
    global PORT
    PORT=args.port;need(PORT and re.fullmatch(r'COM[1-9][0-9]*|/dev/[A-Za-z0-9._/-]+',PORT),'Set --port explicitly')
    need(args.confirm_write,'Serial access requires --confirm-write after reading INSTALL.md')
    version=importlib.metadata.version('esptool');need(version.startswith('4.'),'Use esptool 4.x as documented; no forced tool upgrades')
    name=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    run=Path(args.backup_dir or ROOT/'private-backups')/name;run.mkdir(parents=True,exist_ok=False)
    identity=command(run,'chip-id',['flash_id'],reader=True,before='default_reset')
    need('ESP32-C3' in identity and 'Detected flash size: 8MB' in identity,'This package requires ESP32-C3 with 8MiB flash')
    # Full per-device backup is private; never distribute another person's dump.
    old=read_range(run,'full-before.bin',0,FLASH_BYTES)
    save(run/'backup-receipt.json',{'bytes':len(old),'sha256':sha(old),'private':True,'port':PORT})
    if args.upgrade:
        need(old[0x8000:0x8000+len(parts['partitions'][1])]==parts['partitions'][1],
             'Existing layout differs. No writes made; use the complete install/recovery mode, not app-only upgrade.')
        selected=[role for role in ['application','resources'] if role in parts]
    else:
        selected=[role for role in ['application','resources','bootloader','partitions'] if role in parts]
    # A failure retains all evidence and never replays this transaction.
    save(run/'write-started.json',{'product':m['product'],'version':m['version'],'components':selected,'automaticRetry':False})
    for role in selected:
        path,data,part=parts[role]
        command(run,'write-'+role,['write_flash','--flash_mode','keep','--flash_freq','keep','--flash_size','keep',hex(part['offset']),str(path)])
        actual=read_range(run,'after-'+role+'.bin',part['offset'],len(data))
        need(actual==data,'Readback mismatch; do not boot')
    settings=read_range(run,'settings-after.bin',SETTINGS[0],SETTINGS[1]-SETTINGS[0])
    vendor=read_range(run,'vendor-gap-after.bin',VENDOR[0],VENDOR[1]-VENDOR[0])
    need(settings==old[SETTINGS[0]:SETTINGS[1]],'Settings changed before boot; STOP')
    need(vendor==old[VENDOR[0]:VENDOR[1]],'Identity/reserved data changed; STOP')
    save(run/'result.json',{'product':m['product'],'version':m['version'],'flashed':True,'readbacksIdentical':True,
        'nvsAndPhyIdenticalBeforeBoot':True,'vendorGapIdentical':True,'fullChipErase':False,'restoredOriginalIdentity':False})
    command(run,'start',['flash_id'],reader=True,after='hard_reset')
    print('Verified installation completed. Configure Wi-Fi if required. Identity data already overwritten before this operation cannot be recreated.',flush=True)

def main():
    p=argparse.ArgumentParser(description=__doc__);g=p.add_mutually_exclusive_group();g.add_argument('--install',action='store_true');g.add_argument('--upgrade',action='store_true');p.add_argument('--confirm-write',action='store_true');p.add_argument('--port');p.add_argument('--backup-dir');args=p.parse_args()
    m,parts=validate()
    if not args.install and not args.upgrade:
        print(json.dumps({'passed':True,'product':m['product'],'version':m['version'],'serialAccessed':False,
            'components':[{k:r[2][k] for k in ['role','file','offset','bytes']} for r in parts.values()],'cardidProtected':True},ensure_ascii=False));return
    flash(m,parts,args)

if __name__=='__main__':
    try:main()
    except Exception as e:print('STOP: '+str(e),file=sys.stderr);sys.exit(1)
