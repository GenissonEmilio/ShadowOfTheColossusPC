"""Select a Linux OpenGL adapter for the game process."""
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
        if driver == "nouveau" and re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]", slot):
            return {"DRI_PRIME": "pci-" + slot.replace(":", "_").replace(".", "_")}
    return {}


def prefer_discrete_gpu(environment):
    if EXPLICIT_SELECTION.intersection(environment):
        return
    gpus = switcheroo_gpus()
    if gpus is None:
        environment.update(nvidia_environment())
    else:
        environment.update(switcheroo_environment(gpus))
