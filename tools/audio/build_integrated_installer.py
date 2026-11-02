"""Build the complete Deskflow GUI audio-development MSI from a CMake install stage.

This is NOT the separate audio-preview package. Installing it upgrades Deskflow
using its existing upgrade family, installs/starts the existing Deskflow service,
and adds Private-profile/local-subnet firewall exceptions. It never starts the
GUI or audio capture during installation. Review the generated package before
installing; this script only builds and never invokes msiexec.

Example (run from any directory):
  python tools/audio/build_integrated_installer.py --stage build/stage \
    --wix deps/wix314 --crt "C:/.../x64/Microsoft.VC143.CRT"
"""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import uuid
import json
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WIX = "http://schemas.microsoft.com/wix/2006/wi"
UTIL = "http://schemas.microsoft.com/wix/UtilExtension"
FIREWALL = "http://schemas.microsoft.com/wix/FirewallExtension"
UPGRADE_CODE = "027D1C8A-E7A5-4754-BB93-B2D45BFDBDC8"
VERSION = "1.27.11"  # Same audio features, with a reviewed minimal runtime distribution.
for prefix, namespace in [("", WIX), ("util", UTIL), ("firewall", FIREWALL)]:
    ET.register_namespace(prefix, namespace)


def element(parent, tag, *, namespace=WIX, text=None, **attributes):
    result = ET.SubElement(
        parent, f"{{{namespace}}}{tag}", {k: str(v) for k, v in attributes.items()}
    )
    result.text = text
    return result


def identity(prefix: str, relative: str) -> str:
    return prefix + hashlib.sha256(relative.replace("\\", "/").lower().encode()).hexdigest()[:28]


def component_guid(relative: str) -> str:
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "deskflow-integrated-dev/file/" + relative.lower()))


def rtf_escape(text: str) -> str:
    output = []
    for char in text.replace("\r\n", "\n"):
        if char in "\\{}":
            output.append("\\" + char)
        elif char == "\n":
            output.append("\\par\n")
        elif char == "\t":
            output.append("\\tab ")
        elif ord(char) < 128:
            output.append(char)
        else:
            # RTF \u encodes signed UTF-16 units, including surrogate pairs.
            encoded = char.encode("utf-16le")
            for index in range(0, len(encoded), 2):
                unit = int.from_bytes(encoded[index : index + 2], "little")
                output.append(f"\\u{unit if unit < 32768 else unit - 65536}?")
    return "".join(output)


def validate_stage(stage: Path) -> None:
    required = [
        "deskflow.exe", "deskflow-core.exe", "deskflow-daemon.exe", "deskflow-audio-mute-watchdog.exe",
        "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll", "Qt6Svg.dll", "plugins/tls/qopensslbackend.dll",
        "libcrypto-3-x64.dll", "libssl-3-x64.dll", "plugins/platforms/qwindows.dll",
        "LICENSE", "LICENSE_EXCEPTION",
    ]
    missing = [name for name in required if not (stage / name).is_file()]
    for folder in ["translations", "docs"]:
        if not (stage / folder).is_dir() or not any((stage / folder).iterdir()):
            missing.append(folder + "/ (nonempty)")
    if missing:
        raise ValueError("Incomplete Deskflow install stage: " + ", ".join(missing))
    for path in stage.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"Install stage must not contain links: {path}")
        if path.is_file() and path.suffix.lower() == ".dshaudio":
            raise ValueError("Do not package secret invitation files: " + str(path))


def build(args) -> Path:
    stage = args.stage.resolve(strict=True)
    validate_stage(stage)
    output = args.output.resolve()
    if output.suffix.lower() != ".msi":
        raise ValueError("--output must end in .msi")
    output.parent.mkdir(parents=True, exist_ok=True)
    work = output.parent / (output.stem + "-build")
    work.mkdir(parents=True, exist_ok=True)
    # Keep runtime/licenses intact. Source and build recipes are delivered next
    # to the MSI, never installed into Program Files. Do not mutate the stage.
    source_prefixes = ("docs/third-party/sources/", "docs/third-party/qt-build-config/",
                       "docs/third-party/openssl-build-config/")
    payload = {p.relative_to(stage).as_posix(): p for p in stage.rglob("*")
               if p.is_file() and not p.relative_to(stage).as_posix().startswith(source_prefixes)
               and p.relative_to(stage).as_posix() != "docs/CorrespondingSource.zip"}
    if args.crt:
        crt = args.crt.resolve(strict=True)
        if "x64" not in [part.lower() for part in crt.parts]:
            raise ValueError("--crt must be the redistributable x64 CRT directory")
        for dll in crt.glob("*.dll"):
            payload[dll.name] = dll
    for required in ["vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"]:
        if required not in payload:
            raise ValueError("Missing app-local runtime: " + required + "; supply --crt")
    for path in (ROOT / "docs/dev/audio-licenses").iterdir():
        if path.is_file():
            payload["docs/audio-licenses/" + path.name] = path
    archive = args.source_zip.resolve(strict=True)
    if archive.suffix.lower() != ".zip":
        raise ValueError("--source-zip must be a ZIP of corresponding source")
    # Validate exact third-party sources before publishing the companion archive.
    third_party = stage / "docs/third-party"
    manifest = json.loads((third_party / "SOURCE-MANIFEST.json").read_text(encoding="utf-8"))
    for entry in manifest:
        candidates = list((third_party / "sources").glob("*" + entry["version"] + "*"))
        assert any(p.stat().st_size == entry["bytes"] and hashlib.sha256(p.read_bytes()).hexdigest() == entry["sha256"]
                   for p in candidates), "Missing/mismatched third-party source: " + entry["module"]
    source_bundle = output.with_name(output.stem + "-Sources.zip")
    readme = ("Corresponding source companion for " + output.name + "\n\n"
              "Not needed to install or run Deskflow. Keep this ZIP available alongside the MSI when redistributing.\n"
              "Deskflow-CorrespondingSource.zip contains modified application, Opus/libSRTP and build sources.\n"
              "third-party/ contains exact Qt/OpenSSL sources, licenses, build metadata and hash manifest.\n"
              "Extract the nested application ZIP to build; see BUILD-DEPENDENCIES.md.\n"
              "No restriction is placed on replacing compatible shared Qt libraries or debugging modifications.\n")
    with zipfile.ZipFile(source_bundle, "w", compression=zipfile.ZIP_STORED) as bundle:
        bundle.writestr("README.txt", readme)
        bundle.write(archive, "Deskflow-CorrespondingSource.zip")
        for path in sorted(third_party.rglob("*")):
            if path.is_file():
                bundle.write(path, "third-party/" + path.relative_to(third_party).as_posix())
        bundle.write(stage / "LICENSE", "LICENSE")
        bundle.write(stage / "LICENSE_EXCEPTION", "LICENSE_EXCEPTION")
    with zipfile.ZipFile(source_bundle) as bundle:
        assert bundle.testzip() is None
    source_notice = work / "CorrespondingSource.txt"
    source_notice.write_text(readme + "\nSource ZIP SHA256: " + hashlib.sha256(source_bundle.read_bytes()).hexdigest() + "\n",
                             encoding="utf-8")
    payload["docs/CorrespondingSource.txt"] = source_notice
    # The installed notice must not claim that source archives are in Program Files.
    notice_text = (third_party / "THIRD-PARTY-NOTICES.txt").read_text(encoding="utf-8")
    notice_text = ("SOURCE DISTRIBUTION LAYOUT\nSources/build recipes mentioned below are in the separate companion ZIP: "
                   + source_bundle.name + ".\nLicense texts remain installed. Paths sources/, qt-build-config/ and "
                   "openssl-build-config/ below refer to that ZIP's third-party/ directory, not this installation.\n\n"
                   + notice_text.replace("are INCLUDED in sources/", "are SUPPLIED in companion sources/")
                                .replace("source is also included in sources/", "source is also supplied in companion sources/"))
    installed_notice = work / "THIRD-PARTY-NOTICES.txt"
    installed_notice.write_text(notice_text, encoding="utf-8")
    payload["docs/third-party/THIRD-PARTY-NOTICES.txt"] = installed_notice
    print("Separate source companion:", source_bundle)
    notice = work / "AudioDevelopment.txt"
    notice.write_text(
        "Deskflow Audio Development - 完整桌面程序开发版\n\n"
        "本包保留 deskflow.exe / deskflow-core.exe / deskflow-daemon.exe、Qt、插件、翻译和文档。\n"
        "从开始菜单 Deskflow 启动桌面界面。键鼠配对后音频直接可用，无需单独授权或邀请文件。\n"
        "这是未签名开发构建，不是正式 Deskflow 发布，不保证物理延迟或跨机长期稳定性。\n"
        "安装会升级同系列的旧 Deskflow、配置并启动原 Deskflow 服务；可能中断当前键鼠连接。\n"
        "仅为专用网络/本地子网添加键鼠程序 TCP、音频控制 TCP24801 与音频 UDP 防火墙例外。\n"
        "在插耳机的电脑勾选“在本机收听其他电脑”，已配对的对端自动发送声音；关闭开关停止音频。\n"
        "发送端新增“共享时本机也播放”，默认关闭并记住；共享成功后临时静音本机，停止后恢复。\n"
        "音频开关会记住；键鼠重连后自动恢复。不采麦克风、不录音、不修改系统音量数值。\n"
        "卸载会移除本包服务和防火墙例外，但保留原用户配置。\n"
        "安装前请主动退出当前 Deskflow，保存工作。\n",
        encoding="utf-8",
    )
    payload["docs/AudioDevelopment.txt"] = notice
    license_text = (stage / "LICENSE").read_text(encoding="utf-8")
    exception_text = (stage / "LICENSE_EXCEPTION").read_text(encoding="utf-8")
    license_rtf = work / "License.rtf"
    license_rtf.write_text(
        r"{\rtf1\ansi\deff0{\fonttbl{\f0 Segoe UI;}}\f0\fs18 "
        + rtf_escape("Deskflow Audio Development (unsigned development build)\n\n" + license_text
                     + "\n\nLICENSE EXCEPTION\n\n" + exception_text)
        + "}", encoding="ascii",
    )
    wix = ET.Element(f"{{{WIX}}}Wix")
    product = element(
        wix, "Product", Id="*", Name="Deskflow Audio Development", Version=VERSION,
        Manufacturer="Deskflow Development", Language="2052", Codepage="936", UpgradeCode=UPGRADE_CODE,
    )
    element(product, "Package", InstallerVersion="500", Compressed="yes", InstallScope="perMachine",
            InstallPrivileges="elevated", Platform="x64", SummaryCodepage="936",
            Description="Complete Deskflow desktop application with experimental audio integration")
    element(product, "MajorUpgrade", Schedule="afterInstallInitialize",
            DowngradeErrorMessage="已安装更高版本的 Deskflow；请勿用此开发包降级。")
    element(product, "MediaTemplate", EmbedCab="yes", CompressionLevel="high")
    element(product, "Condition", Message="此开发构建需要64位Windows，面向Windows 10/11。",
            text="Installed OR VersionNT64")
    element(product, "Property", Id="ARPCOMMENTS", Value="Unsigned development build with experimental audio. Upgrades Deskflow.")
    element(product, "SetProperty", Id="ARPINSTALLLOCATION", Value="[INSTALLFOLDER]", After="CostFinalize", Sequence="execute")
    root = element(product, "Directory", Id="TARGETDIR", Name="SourceDir")
    program_files = element(root, "Directory", Id="ProgramFiles64Folder")
    install = element(program_files, "Directory", Id="INSTALLFOLDER", Name="Deskflow")
    menu = element(element(root, "Directory", Id="ProgramMenuFolder"), "Directory",
                   Id="DeskflowMenuFolder", Name="Deskflow")
    directories = {"": install}

    def directory(relative: str):
        if relative in directories:
            return directories[relative]
        path = Path(relative)
        parent = "" if path.parent == Path(".") else path.parent.as_posix()
        result = element(directory(parent), "Directory", Id=identity("Dir", relative), Name=path.name)
        directories[relative] = result
        return result

    feature = element(product, "Feature", Id="CompleteDeskflow", Title="Deskflow完整桌面程序及音频开发功能", Level="1")
    for relative, source in sorted(payload.items()):
        path = Path(relative)
        parent = "" if path.parent == Path(".") else path.parent.as_posix()
        comp_id = identity("Cmp", relative)
        file_id = identity("Fil", relative)
        component = element(directory(parent), "Component", Id=comp_id,
                            Guid=component_guid(relative), Win64="yes")
        element(component, "File", Id=file_id, Name=path.name, Source=str(source), KeyPath="yes")
        if relative == "deskflow-daemon.exe":
            service = element(component, "ServiceInstall", Id="DeskflowServiceInstall", Name="Deskflow",
                              DisplayName="Deskflow", Description="Runs the Core process on secure desktops (UAC prompts, login screen, etc).",
                              ErrorControl="normal", Start="auto", Type="ownProcess", Account="LocalSystem")
            element(service, "ServiceConfig", namespace=UTIL, ResetPeriodInDays="1", RestartServiceDelayInSeconds="1",
                    FirstFailureActionType="restart", SecondFailureActionType="restart", ThirdFailureActionType="none")
            element(component, "ServiceControl", Id="DeskflowServiceControl", Name="Deskflow", Remove="uninstall",
                    Start="install", Stop="both", Wait="yes")
        if relative in ["deskflow-core.exe", "deskflow.exe"]:
            is_core = relative == "deskflow-core.exe"
            attributes = dict(Id="DeskflowCorePrivateTcp" if is_core else "DeskflowAudioPrivateUdp",
                              Name="Deskflow Keyboard Mouse (Private LAN)" if is_core else "Deskflow Audio Development (Private LAN)",
                              File=file_id, Protocol="tcp" if is_core else "udp", Scope="localSubnet", Profile="private",
                              Description="Private profile and local subnet only; removed when Deskflow is uninstalled.")
            # Program-scoped TCP permits Deskflow's configured core port,
            # including non-default ports; Private/localSubnet remains required.
            element(component, "FirewallException", namespace=FIREWALL, **attributes)
            if not is_core:
                element(component, "FirewallException", namespace=FIREWALL,
                        Id="DeskflowAudioPairedControl", Name="Deskflow Paired Audio Control (Private LAN)",
                        File=file_id, Protocol="tcp", Port="24801", Scope="localSubnet", Profile="private",
                        Description="Reuses the existing Deskflow paired identity; no separate audio pairing.")
        element(feature, "ComponentRef", Id=comp_id)
    shortcuts = element(menu, "Component", Id="DeskflowMenuShortcuts",
                        Guid="DC8C6D23-39F9-4CB3-8DD0-3EC0E6718CD6", Win64="yes")
    element(shortcuts, "RegistryValue", Root="HKLM", Key="Software\\Deskflow\\AudioDevelopment",
            Name="Installed", Type="integer", Value="1", KeyPath="yes")
    element(shortcuts, "Shortcut", Id="DeskflowGuiShortcut", Name="Deskflow", Target="[INSTALLFOLDER]deskflow.exe",
            WorkingDirectory="INSTALLFOLDER", Description="Deskflow桌面界面（含实验性音频共享）")
    element(shortcuts, "Shortcut", Id="DeskflowAudioNotesShortcut", Name="音频开发版说明",
            Target="[INSTALLFOLDER]docs\\AudioDevelopment.txt", WorkingDirectory="INSTALLFOLDER")
    element(shortcuts, "RemoveFolder", Id="RemoveDeskflowMenu", On="uninstall")
    element(feature, "ComponentRef", Id="DeskflowMenuShortcuts")
    # Prompt before asking a running GUI to close; never forcibly terminate it.
    close = element(product, "CloseApplication", namespace=UTIL, Id="CloseDeskflowGui", Target="deskflow.exe",
                    PromptToContinue="yes", Description="安装需要关闭正在运行的 Deskflow。请先保存工作并退出，然后重试。",
                    CloseMessage="yes", RebootPrompt="yes", Timeout="5")
    close.text = 'NOT REMOVE~="ALL"'
    element(product, "Property", Id="WIXUI_INSTALLDIR", Value="INSTALLFOLDER")
    element(product, "UIRef", Id="WixUI_InstallDir")
    element(product, "WixVariable", Id="WixUILicenseRtf", Value=str(license_rtf))
    wxs = work / "DeskflowAudioDevelopment.wxs"
    ET.ElementTree(wix).write(wxs, encoding="utf-8", xml_declaration=True)
    print("WiX source:", wxs)
    print("Payload files:", len(payload))
    if args.emit_only:
        return wxs
    wix_dir = args.wix.resolve(strict=True)
    obj = work / "DeskflowAudioDevelopment.wixobj"
    extensions = ["-ext", "WixUtilExtension", "-ext", "WixFirewallExtension"]
    subprocess.run([str(wix_dir / "candle.exe"), "-nologo", "-arch", "x64", *extensions, "-out", str(obj), str(wxs)], check=True)
    # WiX3 FirewallExtension has no zh-cn strings; supply explicit translations.
    localization = ET.Element('{http://schemas.microsoft.com/wix/2006/localization}WixLocalization', Culture='zh-cn', Codepage='936')
    for key, value in {
        'WixSchedFirewallExceptionsInstall': '正在安排添加专用网络防火墙规则',
        'WixSchedFirewallExceptionsUninstall': '正在安排移除防火墙规则',
        'WixRollbackFirewallExceptionsInstall': '正在回滚防火墙规则',
        'WixExecFirewallExceptionsInstall': '正在添加专用网络防火墙规则',
        'WixRollbackFirewallExceptionsUninstall': '正在恢复防火墙规则',
        'WixExecFirewallExceptionsUninstall': '正在移除防火墙规则',
        'msierrFirewallCannotConnect': '无法连接到 Windows 防火墙服务。',
    }.items():
        ET.SubElement(localization, '{http://schemas.microsoft.com/wix/2006/localization}String', Id=key).text = value
    loc = work / 'Firewall.zh-cn.wxl'
    ET.ElementTree(localization).write(loc, encoding='utf-8', xml_declaration=True)
    subprocess.run([str(wix_dir / "light.exe"), "-nologo", '-loc', str(loc), *(["-sval"] if args.skip_ice else []),
                    "-ext", "WixUIExtension", *extensions, "-cultures:zh-cn", "-out", str(output), str(obj)], check=True)
    print("Installer:", output)
    print("SHA256:", hashlib.sha256(output.read_bytes()).hexdigest())
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True, help="Complete cmake --install stage, executables at root")
    parser.add_argument("--wix", type=Path, required=True, help="WiX3.14.1 binary directory")
    parser.add_argument("--crt", type=Path, help="Redistributable x64 Microsoft.VC143.CRT directory, unless already in stage")
    parser.add_argument("--source-zip", type=Path, required=True, help="Application source ZIP; published in separate companion, never installed")
    parser.add_argument("--output", type=Path, default=ROOT / f"build/Deskflow-Audio-Unified-{VERSION}-win-x64.msi")
    parser.add_argument("--skip-ice", action="store_true", help="Explicitly skip ICE only when host Windows Installer service is unavailable")
    parser.add_argument("--emit-only", action="store_true", help="Emit WiX source only, without invoking build tools")
    args = parser.parse_args()
    build(args)


if __name__ == "__main__":
    main()
