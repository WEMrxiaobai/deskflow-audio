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

# 从实际 MSI 表核对清理操作，不能仅凭 WiX 源码中存在清理节点就认定卸载已覆盖。
# 3649 = DLL + continue + commit + in-script + no-impersonate；3137 为对应 deferred 分支。
cleanup_actions = {row[0]: row for row in rows('CustomAction')}
cleanup_schedule = {row[0]: row for row in rows('InstallExecuteSequence')}
for action, action_type, rollback_condition in [
    ('DeskflowCleanupStartupCommit', 3649, 'NOT RollbackDisabled'),
    ('DeskflowCleanupStartupNoRollback', 3137, 'RollbackDisabled'),
]:
    actual = cleanup_actions[action]
    assert int(actual[1]) == action_type, actual
    assert actual[2:4] == ['DeskflowStartupCleanupBinary', 'CleanDeskflowAutoStart'], actual
    condition = 'Installed AND REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE AND ' + rollback_condition
    assert cleanup_schedule[action][1] == condition, cleanup_schedule[action]
    assert (int(cleanup_schedule['RemoveFiles'][2]) < int(cleanup_schedule[action][2])
            < int(cleanup_schedule['InstallFinalize'][2])), cleanup_schedule[action]
    # CustomActionData 必须在对应操作排入执行脚本前准备，且只能传本安装目录的目标路径。
    setter = cleanup_actions['Set' + action]
    assert setter[1:4] == ['51', action, '[INSTALLFOLDER]deskflow.exe'], setter
    assert cleanup_schedule['Set' + action][1] == condition
    assert int(cleanup_schedule['Set' + action][2]) < int(cleanup_schedule[action][2])
print('Verified final-uninstall startup cleanup, upgrade exclusion and rollback scheduling')

files=rows('File');names={row[2].split('|')[-1] for row in files}
required={'deskflow-audio-mute-watchdog.exe','deskflow.exe','deskflow-core.exe','deskflow-daemon.exe','Qt6Core.dll','Qt6Widgets.dll','Qt6Gui.dll','Qt6Network.dll','Qt6Svg.dll','libssl-3-x64.dll','libcrypto-3-x64.dll','qwindows.dll','qopensslbackend.dll','deskflow_zh_CN.qm','vcruntime140.dll','msvcp140.dll'}
assert required<=names,required-names
assert not any(name.endswith('.cmd') for name in names),'Main product must not launch through preview CMD scripts'
shortcuts=rows('Shortcut');assert any(row[4]=='[INSTALLFOLDER]deskflow.exe' for row in shortcuts)
assert len(rows('Dialog'))>=20
services=rows('ServiceInstall');assert any(row[1]=='Deskflow' for row in services)
firewall=rows('WixFirewallException');assert len(firewall)==3
properties=dict((row[0],row[1]) for row in rows('Property'))
if tuple(map(int,properties['ProductVersion'].split('.'))) >= (1,27,6):
    # 检查编译后的 Shortcut/Directory/Component/FeatureComponents 表，而非只检查 WiX 源码。
    # 入口必须落在标准桌面、直接指向 GUI，且归属于已安装功能，避免“有记录但组件不安装”。
    desktop=[row for row in shortcuts if row[0]=='DeskflowDesktopShortcut']
    assert len(desktop)==1, 'Missing or duplicate desktop shortcut'
    shortcut=desktop[0]
    assert shortcut[1:3]==['DesktopFolder','Deskflow'], shortcut
    assert shortcut[4]=='[INSTALLFOLDER]deskflow.exe' and shortcut[11]=='INSTALLFOLDER', shortcut
    assert any(row[0]=='DesktopFolder' and row[1]=='TARGETDIR' for row in rows('Directory'))
    assert any(row[1]==shortcut[3] for row in rows('FeatureComponents')), 'Desktop component is not installed'
    component=next(row for row in rows('Component') if row[0]==shortcut[3])
    registry=next(row for row in rows('Registry') if row[0]==component[5])
    assert int(component[3]) & 4 and registry[1]=='1', 'Shortcut component requires an HKCU registry KeyPath'
    # 安装与卸载分别交由标准动作管理快捷方式，不额外运行脚本，不移除用户的桌面目录。
    actions={row[0] for row in rows('InstallExecuteSequence')}
    assert {'CreateShortcuts','RemoveShortcuts'}<=actions, 'Missing shortcut lifecycle actions'
    print('Verified desktop shortcut target, feature ownership and install/uninstall actions')
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
