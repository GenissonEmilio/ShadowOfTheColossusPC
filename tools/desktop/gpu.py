"""Request an OpenGL adapter for the game process."""
from pathlib import Path
import re

OPENGL_SELECTORS = {
    "DRI_PRIME",
    "__NV_PRIME_RENDER_OFFLOAD",
    "__NV_PRIME_RENDER_OFFLOAD_PROVIDER",
    "__GLX_VENDOR_LIBRARY_NAME",
}
EXPLICIT_SELECTION = OPENGL_SELECTORS | {
    "LIBGL_ALWAYS_SOFTWARE", "LIBGL_ALWAYS_INDIRECT", "MESA_LOADER_DRIVER_OVERRIDE",
}
SWITCHEROO = "net.hadess.SwitcherooControl"
PCI_SLOT = re.compile(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]")


def mesa_pci_environment(slot):
    return {"DRI_PRIME": "pci-" + slot.replace(":", "_").replace(".", "_")}


def switcheroo_gpus():
    try:
        from gi.repository import Gio, GLib
    except ImportError:
        return None
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)
        reply = bus.call_sync(
            SWITCHEROO, "/net/hadess/SwitcherooControl",
            "org.freedesktop.DBus.Properties", "Get",
            GLib.Variant("(ss)", (SWITCHEROO, "GPUs")),
            GLib.VariantType.new("(v)"), Gio.DBusCallFlags.NO_AUTO_START, 1000, None,
        )
        return reply.unpack()[0]
    except GLib.Error:
        return None


def switcheroo_environment(gpus):
    discrete = [gpu for gpu in gpus if gpu.get("Discrete") is True]
    if not discrete and all("Discrete" not in gpu for gpu in gpus):
        # Older switcheroo-control uses the first non-default device for offload.
        discrete = [gpu for gpu in gpus if gpu.get("Default") is False]
    if any(gpu.get("Default") is True for gpu in discrete):
        return {}
    for gpu in discrete:
        pairs = gpu.get("Environment", [])
        if len(pairs) % 2:
            continue
        settings = {key: value for key, value in zip(pairs[::2], pairs[1::2])
                    if key in OPENGL_SELECTORS and value}
        if settings:
            return settings
    return {}


def nvidia_environment(drm=Path("/sys/class/drm")):
    # On systems without switcheroo-control, select a secondary NVIDIA PCI
    # adapter with a loaded driver. A dedicated boot GPU already handles GL.
    devices = {render.resolve() for render in drm.glob("renderD[0-9]*")}
    if len(devices) < 2:
        return {}
    for render in sorted(devices):
        device = render / "device"
        try:
            if (device / "vendor").read_text().strip() != "0x10de":
                continue
            if (device / "boot_vga").read_text().strip() != "0":
                continue
            driver = (device / "driver").resolve(strict=True).name
            slot = device.resolve(strict=True).name
        except OSError:
            continue
        if driver == "nvidia":
            return {"__NV_PRIME_RENDER_OFFLOAD": "1", "__GLX_VENDOR_LIBRARY_NAME": "nvidia"}
        if driver == "nouveau" and PCI_SLOT.fullmatch(slot):
            return mesa_pci_environment(slot)
    return {}


def amd_is_integrated(node):
    import ctypes
    import fcntl
    import struct

    # Linux amdgpu_drm.h: DRM_IOCTL_AMDGPU_INFO on x86-64, requesting
    # AMDGPU_INFO_DEV_INFO. The kernel accepts the prefix through ids_flags.
    ioctl_info = 0x40206445
    dev_info = 0x16
    fusion_flag = 0x01
    result = ctypes.create_string_buffer(144)
    request = struct.pack("=QII16x", ctypes.addressof(result), len(result), dev_info)
    try:
        with node.open("rb", buffering=0) as device:
            fcntl.ioctl(device, ioctl_info, request)
    except OSError:
        return None
    device_id, = struct.unpack_from("=I", result)
    ids_flags, = struct.unpack_from("=Q", result, 136)
    if not device_id:
        return None
    return bool(ids_flags & fusion_flag)


def amd_environment(drm=Path("/sys/class/drm"), nodes=Path("/dev/dri")):
    # Offload an AMD hybrid with one discrete Radeon and an integrated boot
    # Radeon. Firmware's boot_vga flag alone cannot distinguish the two.
    boot_nodes = []
    secondary_nodes = []
    for render in drm.glob("renderD[0-9]*"):
        try:
            device = (render / "device").resolve(strict=True)
            if not PCI_SLOT.fullmatch(device.name) or (device / "vendor").read_text().strip() != "0x1002":
                continue
            if (device / "driver").resolve(strict=True).name != "amdgpu":
                continue
            boot = (device / "boot_vga").read_text().strip()
        except OSError:
            continue
        if boot == "1":
            boot_nodes.append(nodes / render.name)
        elif boot == "0":
            secondary_nodes.append((device.name, nodes / render.name))
    if len(boot_nodes) != 1 or not secondary_nodes or amd_is_integrated(boot_nodes[0]) is not True:
        return {}
    discrete = {slot for slot, node in secondary_nodes if amd_is_integrated(node) is False}
    if len(discrete) == 1:
        return mesa_pci_environment(discrete.pop())
    return {}


def prefer_discrete_gpu(environment):
    if EXPLICIT_SELECTION.intersection(environment):
        return
    gpus = switcheroo_gpus()
    if gpus is None:
        environment.update(nvidia_environment() or amd_environment())
    else:
        environment.update(switcheroo_environment(gpus))


def prefer_windows_gpu(executable):
    import winreg

    # Windows Graphics settings are keyed by the absolute render process path.
    # Keep entries the user already configured, even if they prefer the iGPU.
    name = str(executable.resolve())
    settings = r"Software\Microsoft\DirectX\UserGpuPreferences"
    try:
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, settings, 0,
                               winreg.KEY_QUERY_VALUE | winreg.KEY_SET_VALUE) as key:
            try:
                winreg.QueryValueEx(key, name)
            except FileNotFoundError:
                winreg.SetValueEx(key, name, 0, winreg.REG_SZ, "GpuPreference=2;")
                return "requested high performance for game.exe"
            return "kept existing Windows preference for game.exe"
    except OSError as error:
        code = getattr(error, "winerror", error.errno)
        return f"Windows graphics preference unavailable ({code}). Driver preference remains active."
