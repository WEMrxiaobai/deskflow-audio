"""Read a complete Deskflow MSI's actual database without installing it."""
from pathlib import Path
import argparse,ctypes,json,hashlib,xml.etree.ElementTree as E
from ctypes import wintypes as W
p=argparse.ArgumentParser();p.add_argument('msi',type=Path);p.add_argument('--stage',type=Path,required=True);args=p.parse_args()
m=ctypes.WinDLL('msi');db=W.UINT()
m.MsiOpenDatabaseW.argtypes=[W.LPCWSTR,W.LPCWSTR,ctypes.POINTER(W.UINT)]
m.MsiDatabaseOpenViewW.argtypes=[W.UINT,W.LPCWSTR,ctypes.POINTER(W.UINT)]
m.MsiRecordGetStringW.argtypes=[W.UINT,W.UINT,W.LPWSTR,ctypes.POINTER(W.DWORD)]
m.MsiRecordReadStream.argtypes=[W.UINT,W.UINT,ctypes.c_void_p,ctypes.POINTER(W.DWORD)]
assert m.MsiOpenDatabaseW(str(args.msi.resolve()),None,ctypes.byref(db))==0

def rows(table):
    view=W.UINT();assert m.MsiDatabaseOpenViewW(db,'SELECT * FROM '+table,ctypes.byref(view))==0
    assert m.MsiViewExecute(view,0)==0
    result=[];rec=W.UINT()
    while m.MsiViewFetch(view,ctypes.byref(rec))==0:
        fields=[]
        for i in range(1,m.MsiRecordGetFieldCount(rec)+1):
            text=ctypes.create_unicode_buffer(32768);size=W.DWORD(32768)
            assert m.MsiRecordGetStringW(rec,i,text,ctypes.byref(size))==0
            fields.append(text.value)
        result.append(fields);m.MsiCloseHandle(rec)
    m.MsiCloseHandle(view);return result
files=rows('File');names={row[2].split('|')[-1] for row in files}
required={'deskflow-audio-mute-watchdog.exe','deskflow.exe','deskflow-core.exe','deskflow-daemon.exe','Qt6Core.dll','Qt6Widgets.dll','Qt6Gui.dll','Qt6Network.dll','Qt6Svg.dll','libssl-3-x64.dll','libcrypto-3-x64.dll','qwindows.dll','qopensslbackend.dll','deskflow_zh_CN.qm','vcruntime140.dll','msvcp140.dll'}
assert required<=names,required-names
assert not any(name.endswith('.cmd') for name in names),'Main product must not launch through preview CMD scripts'
shortcuts=rows('Shortcut');assert any(row[4]=='[INSTALLFOLDER]deskflow.exe' for row in shortcuts)
assert len(rows('Dialog'))>=20
services=rows('ServiceInstall');assert any(row[1]=='Deskflow' for row in services)
firewall=rows('WixFirewallException');assert len(firewall)==3
properties=dict((row[0],row[1]) for row in rows('Property'))
print(json.dumps({'product':properties['ProductName'],'version':properties['ProductVersion'],'files':len(files),'shortcuts':len(shortcuts),'services':len(services),'firewall_rules':len(firewall),'sha256':hashlib.sha256(args.msi.read_bytes()).hexdigest()},ensure_ascii=True,indent=2))
# Verify actual compressed executable bytes, not only the File table entries.
import subprocess
media=rows('Media');cab_name=media[0][3].lstrip('#')
view=W.UINT();assert m.MsiDatabaseOpenViewW(db,"SELECT `Data` FROM `_Streams` WHERE `Name`='"+cab_name+"'",ctypes.byref(view))==0
assert m.MsiViewExecute(view,0)==0
record=W.UINT();assert m.MsiViewFetch(view,ctypes.byref(record))==0
work=args.msi.resolve().parent/(args.msi.stem+'-verified-cabinet');work.mkdir(exist_ok=True)
cab=work/'payload.cab'
with cab.open('wb') as out:
    while True:
        buffer=ctypes.create_string_buffer(65536);size=W.DWORD(65536)
        assert m.MsiRecordReadStream(record,1,buffer,ctypes.byref(size))==0
        if not size.value:break
        out.write(buffer.raw[:size.value])
m.MsiCloseHandle(record);m.MsiCloseHandle(view)
for name in ['deskflow.exe','deskflow-core.exe','deskflow-daemon.exe','deskflow-audio-mute-watchdog.exe']:
    row=next(row for row in files if row[2].split('|')[-1]==name)
    subprocess.run(['expand.exe',str(cab),'-F:'+row[0],str(work)],check=True)
    extracted=work/row[0]
    assert hashlib.sha256(extracted.read_bytes()).digest()==hashlib.sha256((args.stage/name).read_bytes()).digest(),name+' payload differs from built binary'
    print('Verified embedded build:',name)
# Reconstruct an install layout directly from the CAB for dependency tests. This
# does NOT run MSI actions, create services, or touch the installed Deskflow.
import shutil,os
components={row[0]:row[2] for row in rows('Component')}
directories={row[0]:(row[1],row[2]) for row in rows('Directory')}
layout=work/'layout';layout.mkdir(exist_ok=True)
def relative_dir(directory):
    if directory=='INSTALLFOLDER':return Path()
    parent,name=directories[directory]
    assert parent, directory
    return relative_dir(parent)/name.split('|')[-1]
subprocess.run(['expand.exe',str(cab),'-F:*',str(work)],check=True,stdout=subprocess.DEVNULL)
for row in files:
    destination=layout/relative_dir(components[row[1]])/row[2].split('|')[-1]
    destination.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(work/row[0],destination)
# Exact runtime/license parity: source separation must never trim executable deps.
installed={row[2].split('|')[-1] for row in files}
for source in args.stage.rglob('*'):
    if source.is_file() and source.suffix.lower() in {'.exe','.dll','.qm'}:
        destination=layout/source.relative_to(args.stage)
        assert destination.is_file(), 'Missing runtime file: '+str(source)
        assert hashlib.sha256(destination.read_bytes()).digest()==hashlib.sha256(source.read_bytes()).digest(),str(source)
assert (layout/'LICENSE').is_file() and (layout/'LICENSE_EXCEPTION').is_file()
if tuple(map(int,properties['ProductVersion'].split('.'))) >= (1,27,4):
    assert not any(name.endswith(('.tar.gz','.tar.xz','.zip')) for name in installed), 'Source archive leaked into runtime MSI'
    assert (layout/'docs/CorrespondingSource.txt').is_file()
print('All staged EXE/DLL/translation bytes verified; licenses retained.')
env={key.upper():value for key,value in os.environ.items()};env['PATH']=env['SYSTEMROOT']+'\\System32;'+env['SYSTEMROOT'];env.pop('QT_PLUGIN_PATH',None)
for name in ['deskflow.exe','deskflow-core.exe']:
    result=subprocess.run([str(layout/name),'--version'],cwd=layout,env=env,check=True,timeout=20)
    print('Clean-path startup passed:',name)
print('Full CAB layout:',layout)
m.MsiCloseHandle(db)
