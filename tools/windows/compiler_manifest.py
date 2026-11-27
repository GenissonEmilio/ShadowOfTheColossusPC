"""Use UTF-8 throughout the staged Windows toolchain."""
from __future__ import annotations

import ctypes
from ctypes import wintypes
from pathlib import Path
from xml.etree import ElementTree as ET


def parse_manifest(data):
    # Resource padding is NUL, but a UTF-16 character's high/low byte can
    # also be NUL. Strip complete code units without truncating the XML.
    if data[:2] in (b"\xff\xfe", b"\xfe\xff", b"<\0", b"\0<"):
        while data.endswith(b"\0\0"):
            data = data[:-2]
    else:
        data = data.rstrip(b"\0")
    return ET.fromstring(data)


def enable_utf8(executable):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    pointer, handle, number = ctypes.c_void_p, wintypes.HANDLE, wintypes.DWORD
    enumerate_language = ctypes.WINFUNCTYPE(wintypes.BOOL, handle, pointer, pointer,
                                           wintypes.WORD, wintypes.LPARAM)
    signatures = (
        ("LoadLibraryExW", handle, (wintypes.LPCWSTR, handle, number)),
        ("EnumResourceLanguagesW", wintypes.BOOL, (handle, pointer, pointer, enumerate_language, wintypes.LPARAM)),
        ("FindResourceExW", handle, (handle, pointer, pointer, wintypes.WORD)),
        ("LoadResource", handle, (handle, handle)),
        ("LockResource", pointer, (handle,)),
        ("SizeofResource", number, (handle, handle)),
        ("FreeLibrary", wintypes.BOOL, (handle,)),
        ("BeginUpdateResourceW", handle, (wintypes.LPCWSTR, wintypes.BOOL)),
        ("UpdateResourceW", wintypes.BOOL, (handle, pointer, pointer, wintypes.WORD, pointer, number)),
        ("EndUpdateResourceW", wintypes.BOOL, (handle, wintypes.BOOL)),
    )
    for name, result, arguments in signatures:
        function = getattr(kernel, name)
        function.restype = result
        function.argtypes = arguments

    manifest_type, manifest_id = 24, 1
    namespaces = {
        "assembly": "urn:schemas-microsoft-com:asm.v1",
        "asmv3": "urn:schemas-microsoft-com:asm.v3",
        "path": "http://schemas.microsoft.com/SMI/2016/WindowsSettings",
        "codepage": "http://schemas.microsoft.com/SMI/2019/WindowsSettings",
    }
    ET.register_namespace("", namespaces["assembly"])
    ET.register_namespace("asmv3", namespaces["asmv3"])
    template = ET.parse(Path(__file__).with_name("utf8.manifest")).getroot()
    settings = template.find("asmv3:application/asmv3:windowsSettings", namespaces)
    updates = {}
    languages = []

    @enumerate_language
    def remember_language(_module, _type, _name, language, _context):
        languages.append(language)
        return True

    load_as_datafile, load_as_image_resource = 0x2, 0x20
    module = kernel.LoadLibraryExW(str(executable), None, load_as_datafile | load_as_image_resource)
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not kernel.EnumResourceLanguagesW(module, manifest_type, manifest_id, remember_language, 0):
            error = ctypes.get_last_error()
            # Some tools (including the MSYS2 Clang driver) lack a manifest.
            if error not in (1812, 1813, 1814):
                raise ctypes.WinError(error)
            updates[0] = ET.tostring(template, encoding="utf-8", xml_declaration=True)
        for language in languages:
            resource = kernel.FindResourceExW(module, manifest_type, manifest_id, language)
            if not resource:
                raise ctypes.WinError(ctypes.get_last_error())
            size = kernel.SizeofResource(module, resource)
            address = kernel.LockResource(kernel.LoadResource(module, resource))
            if not size or not address:
                raise ctypes.WinError(ctypes.get_last_error())
            manifest = parse_manifest(ctypes.string_at(address, size))
            application = manifest.find("asmv3:application", namespaces)
            if application is None:
                application = ET.SubElement(manifest, f"{{{namespaces['asmv3']}}}application")
            current_settings = application.find("asmv3:windowsSettings", namespaces)
            if current_settings is None:
                current_settings = ET.SubElement(application, settings.tag)
            changed = False
            for setting in settings:
                current = application.find(f".//{setting.tag}")
                if current is None:
                    current_settings.append(setting)
                    changed = True
                elif current.text != setting.text:
                    current.text = setting.text
                    changed = True
            if changed:
                updates[language] = ET.tostring(manifest, encoding="utf-8", xml_declaration=True)
    finally:
        kernel.FreeLibrary(module)

    if not updates:
        return
    update = kernel.BeginUpdateResourceW(str(executable), False)
    if not update:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        for language, manifest in updates.items():
            if not kernel.UpdateResourceW(update, manifest_type, manifest_id, language, manifest, len(manifest)):
                raise ctypes.WinError(ctypes.get_last_error())
    except BaseException:
        kernel.EndUpdateResourceW(update, True)
        raise
    if not kernel.EndUpdateResourceW(update, False):
        raise ctypes.WinError(ctypes.get_last_error())
