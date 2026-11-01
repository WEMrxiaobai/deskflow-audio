"""Build a real per-user Windows x64 MSI for the explicit audio preview.
Requires WiX 3.14.1 binaries and the VS2022 x64 redistributable CRT directory.
No capture, firewall changes, service, or autostart is performed by the installer.
"""
from pathlib import Path
import argparse
import hashlib
import shutil
import subprocess
import uuid
import zipfile
import xml.etree.ElementTree as E

ROOT = Path(__file__).resolve().parents[2]
NS = 'http://schemas.microsoft.com/wix/2006/wi'
E.register_namespace('', NS)
def node(parent, tag, **attrs):
    return E.SubElement(parent, '{'+NS+'}'+tag, {k: str(v) for k,v in attrs.items()})

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wix', type=Path, required=True)
    parser.add_argument('--crt', type=Path, required=True)
    parser.add_argument('--skip-ice', action='store_true', help='Explicitly skip ICE only when build host Windows Installer service is unavailable')
    args=parser.parse_args()
    build=ROOT/'build/audio/installer'
    payload=build/'payload'
    payload.mkdir(parents=True,exist_ok=True)
    files=[('deskflow-audio-preview.exe', ROOT/'build/audio/Release/deskflow-audio-preview.exe'),
           ('receiver.cmd',ROOT/'src/apps/deskflow-audio-preview/接收远端声音.cmd'),
           ('sender.cmd',ROOT/'src/apps/deskflow-audio-preview/发送系统声音.cmd'),
           ('使用说明.txt',ROOT/'src/apps/deskflow-audio-preview/使用说明.txt'),
           ('Deskflow-LICENSE.txt',ROOT/'LICENSE')]
    for dll in args.crt.glob('*.dll'):
        files.append((dll.name,dll))
    if not any(name=='vcruntime140.dll' for name,_ in files):
        raise RuntimeError('Expected redistributable x64 CRT files not found')
    for license_file in (ROOT/'docs/dev/audio-licenses').iterdir():
        if license_file.is_file():files.append((license_file.name,license_file))
    # Matching source and pinned dependency source accompany the binary.
    source=payload/'CorrespondingSource.zip'
    with zipfile.ZipFile(source,'w',zipfile.ZIP_DEFLATED) as archive:
        for folder in ['src/lib/audio','src/apps/deskflow-audio-preview','src/apps/deskflow-audio','deps/opus-1.5.2','deps/libsrtp-2.7.0']:
            for path in (ROOT/folder).rglob('*'):
                if path.is_file(): archive.write(path,path.relative_to(ROOT))
        archive.write(Path(__file__), 'tools/audio/build_preview_installer.py')
        archive.writestr('BUILD.txt','From source root, with VS2022 C++/Windows SDK/CMake:\ncmake -S src/lib/audio -B build -A x64 -DDESKFLOW_AUDIO_FETCH_DEPENDENCIES=ON -DFETCHCONTENT_SOURCE_DIR_AUDIO_OPUS="%CD%/deps/opus-1.5.2" -DFETCHCONTENT_SOURCE_DIR_AUDIO_SRTP="%CD%/deps/libsrtp-2.7.0"\ncmake --build build --config Release --target deskflow-audio-preview\n')
    files.append((source.name,source))
    for name,original in files:
        target=payload/name
        if target.resolve()!=original.resolve():shutil.copy2(original,target)
    wix=E.Element('{'+NS+'}Wix')
    product=node(wix,'Product',Id='*',Name='Deskflow 音频共享预览',Language='2052',Codepage='936',Version='0.1.0',Manufacturer='Deskflow Audio Preview',UpgradeCode='7ACBF252-A8A8-4EF0-99BE-B6D2AFA71C42')
    node(product,'Package',InstallerVersion='500',Compressed='yes',InstallScope='perUser',InstallPrivileges='limited',Platform='x64',SummaryCodepage='936',Description='Deskflow explicit single-source audio sharing preview')
    node(product,'MajorUpgrade',DowngradeErrorMessage='A newer preview is already installed.')
    node(product,'MediaTemplate',EmbedCab='yes')
    node(product,'Condition',Message='Requires 64-bit Windows; this preview targets Windows 10/11.') .text='Installed OR VersionNT64'
    target=node(product,'Directory',Id='TARGETDIR',Name='SourceDir')
    local=node(target,'Directory',Id='LocalAppDataFolder')
    install=node(local,'Directory',Id='INSTALLFOLDER',Name='DeskflowAudioPreview')
    menu=node(node(target,'Directory',Id='ProgramMenuFolder'),'Directory',Id='PreviewMenuFolder',Name='Deskflow 音频共享预览')
    feature=node(product,'Feature',Id='MainFeature',Title='Audio preview',Level='1')
    for index,(name,_) in enumerate(files):
        ident=f'Payload{index}'
        comp=node(install,'Component',Id=ident,Guid=str(uuid.uuid5(uuid.NAMESPACE_URL,'deskflow-audio-preview/file/'+name)),Win64='yes')
        node(comp,'RegistryValue',Root='HKCU',Key='Software\\DeskflowAudioPreview\\Files',Name=name,Type='integer',Value='1',KeyPath='yes')
        node(comp,'File',Id=f'File{index}',Name=name,Source=str(payload/name),KeyPath='no')
        node(feature,'ComponentRef',Id=ident)
    comp=node(menu,'Component',Id='MenuShortcuts',Guid='A209AB57-3165-4E3D-BF10-74D84C2AC9E5',Win64='yes')
    node(comp,'RegistryValue',Root='HKCU',Key='Software\\DeskflowAudioPreview',Name='Installed',Type='integer',Value='1',KeyPath='yes')
    for ident,name,filename in [('ReceiveShortcut','接收远端声音','receiver.cmd'),('SendShortcut','发送系统声音','sender.cmd'),('ReadmeShortcut','使用说明','使用说明.txt')]:
        node(comp,'Shortcut',Id=ident,Name=name,Target='[INSTALLFOLDER]'+filename,WorkingDirectory='INSTALLFOLDER')
    node(comp,'Shortcut',Id='UninstallShortcut',Name='卸载音频共享预览',Target='[System64Folder]msiexec.exe',Arguments='/x [ProductCode]')
    node(comp,'RemoveFolder',Id='RemovePreviewMenu',On='uninstall')
    node(feature,'ComponentRef',Id='MenuShortcuts')
    node(product,'Property',Id='WIXUI_INSTALLDIR',Value='INSTALLFOLDER')
    node(product,'UIRef',Id='WixUI_InstallDir')
    license_rtf=build/'License.rtf'
    license_rtf.write_text(r'{\rtf1\ansi\deff0 {\fonttbl {\f0 Segoe UI;}}\f0\fs20 Deskflow Audio Sharing - Developer Preview\par\par GPL-2.0-only WITH LicenseRef-OpenSSL-Exception. Full license and third-party notices are installed with this application.\par\par Explicit single-source LAN preview, NOT a production Deskflow release. Invitations contain temporary bearer secrets; transfer only through a trusted channel. No service, autostart, firewall change or automatic capture is installed.\par\par No warranty. Real two-machine audio quality and physical latency are not certified.}',encoding='ascii')
    node(product,'WixVariable',Id='WixUILicenseRtf',Value=str(license_rtf))
    wxs=build/'AudioPreview.wxs'
    E.ElementTree(wix).write(wxs,encoding='utf-8',xml_declaration=True)
    obj=build/'AudioPreview.wixobj'
    output=ROOT/'build/audio/Deskflow-Audio-Preview-0.1.0-x64.msi'
    subprocess.run([str(args.wix/'candle.exe'),'-nologo','-arch','x64','-out',str(obj),str(wxs)],check=True)
    subprocess.run([str(args.wix/'light.exe'),'-nologo',*(['-sval'] if args.skip_ice else []),'-ext','WixUIExtension','-cultures:zh-cn','-out',str(output),str(obj)],check=True)
    print(output)
    print('SHA256',hashlib.sha256(output.read_bytes()).hexdigest())
if __name__=='__main__':main()
