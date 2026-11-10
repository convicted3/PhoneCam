#!/usr/bin/env python3
"""Minimal Android binary XML (AXML) writer for AndroidManifest.xml.

Lets us build an APK without aapt. Supports the attribute kinds a manifest needs:
strings, ints, booleans and references, with android:* attributes mapped to
framework resource ids.
"""
import struct
import sys

ANDROID_NS = "http://schemas.android.com/apk/res/android"

# framework attribute ids (android.R.attr)
ATTR_IDS = {
    "theme": 0x01010000,
    "label": 0x01010001,
    "icon": 0x01010002,
    "name": 0x01010003,
    "exported": 0x01010010,
    "screenOrientation": 0x0101001e,
    "configChanges": 0x0101001f,
    "minSdkVersion": 0x0101020c,
    "versionCode": 0x0101021b,
    "versionName": 0x0101021c,
    "targetSdkVersion": 0x01010270,
    "required": 0x0101028e,
    "hardwareAccelerated": 0x010102d3,
    "allowBackup": 0x01010280,
}

T_REFERENCE = 0x01
T_STRING = 0x03
T_INT_DEC = 0x10
T_INT_HEX = 0x11
T_BOOL = 0x12


class Elem:
    def __init__(self, tag, attrs=None, children=None):
        self.tag = tag
        self.attrs = attrs or []  # list of (ns_is_android, name, type, value)
        self.children = children or []


def a(name, value, android=True):
    """Build an attribute tuple, inferring the type from the python value."""
    if isinstance(value, bool):
        return (android, name, T_BOOL, 0xFFFFFFFF if value else 0)
    if isinstance(value, int):
        return (android, name, T_INT_DEC, value)
    if isinstance(value, tuple) and value[0] == "hex":
        return (android, name, T_INT_HEX, value[1])
    if isinstance(value, tuple) and value[0] == "ref":
        return (android, name, T_REFERENCE, value[1])
    return (android, name, T_STRING, str(value))


def encode(root):
    # collect strings: resource-id'd attribute names first (in id order), then the rest
    res_names = []
    others = []

    def walk(e):
        for (andr, name, t, v) in e.attrs:
            if andr and name in ATTR_IDS:
                if name not in res_names:
                    res_names.append(name)
            elif name not in others:
                others.append(name)
            if t == T_STRING and v not in others:
                others.append(v)
        if e.tag not in others:
            others.append(e.tag)
        for c in e.children:
            walk(c)

    walk(root)
    res_names.sort(key=lambda n: ATTR_IDS[n])
    for s in ("android", ANDROID_NS):
        if s not in others:
            others.append(s)
    strings = res_names + [s for s in others if s not in res_names]
    idx = {s: i for i, s in enumerate(strings)}

    # string pool (UTF-16)
    offsets = []
    data = b""
    for s in strings:
        offsets.append(len(data))
        enc = s.encode("utf-16-le")
        n = len(enc) // 2
        if n > 0x7FFF:
            raise ValueError("string too long")
        data += struct.pack("<H", n) + enc + b"\x00\x00"
    while len(data) % 4:
        data += b"\x00"
    hdr_size = 28
    strings_start = hdr_size + 4 * len(strings)
    sp_size = strings_start + len(data)
    sp = struct.pack("<HHIIIIII", 0x0001, hdr_size, sp_size, len(strings), 0, 0, strings_start, 0)
    sp += b"".join(struct.pack("<I", o) for o in offsets) + data

    # resource map
    rm = struct.pack("<HHI", 0x0180, 8, 8 + 4 * len(res_names))
    rm += b"".join(struct.pack("<I", ATTR_IDS[n]) for n in res_names)

    NONE = 0xFFFFFFFF
    body = b""
    body += struct.pack("<HHIIIII", 0x0100, 16, 24, 1, NONE, idx["android"], idx[ANDROID_NS])

    def emit(e):
        nonlocal body
        attrs = []
        for (andr, name, t, v) in e.attrs:
            ns = idx[ANDROID_NS] if andr else NONE
            raw = idx[v] if t == T_STRING else NONE
            dat = idx[v] if t == T_STRING else (v & 0xFFFFFFFF)
            key = ATTR_IDS.get(name, 0x7FFFFFFF) if andr else 0x7FFFFFFF
            attrs.append((key, struct.pack("<IIIHBBI", ns, idx[name], raw, 8, 0, t, dat)))
        attrs.sort(key=lambda x: x[0])
        ab = b"".join(x[1] for x in attrs)
        size = 16 + 20 + len(ab)
        body += struct.pack("<HHIII", 0x0102, 16, size, 1, NONE)
        body += struct.pack("<IIHHHHHH", NONE, idx[e.tag], 20, 20, len(attrs), 0, 0, 0)
        body += ab
        for c in e.children:
            emit(c)
        body += struct.pack("<HHIIIII", 0x0103, 16, 24, 1, NONE, NONE, idx[e.tag])

    emit(root)
    body += struct.pack("<HHIIIII", 0x0101, 16, 24, 1, NONE, idx["android"], idx[ANDROID_NS])

    content = sp + rm + body
    return struct.pack("<HHI", 0x0003, 8, 8 + len(content)) + content


def phonecam_manifest(package, version_code, version_name, label, icon_ref=None):
    perms = [
        "android.permission.CAMERA",
        "android.permission.INTERNET",
        "android.permission.ACCESS_NETWORK_STATE",
        "android.permission.ACCESS_WIFI_STATE",
        "android.permission.CHANGE_WIFI_MULTICAST_STATE",
        "android.permission.WAKE_LOCK",
    ]
    app_attrs = [a("label", label), a("hardwareAccelerated", True), a("allowBackup", False)]
    if icon_ref:
        app_attrs.append(a("icon", ("ref", icon_ref)))
    activity = Elem("activity", [
        a("name", package + ".MainActivity"),
        a("label", label),
        a("exported", True),
        a("configChanges", ("hex", 0x5B0)),  # keyboard|keyboardHidden|orientation|screenLayout|screenSize
    ], [
        Elem("intent-filter", [], [
            Elem("action", [a("name", "android.intent.action.MAIN")]),
            Elem("category", [a("name", "android.intent.category.LAUNCHER")]),
        ])
    ])
    children = [Elem("uses-sdk", [a("minSdkVersion", 21), a("targetSdkVersion", 29)])]
    children += [Elem("uses-permission", [a("name", p)]) for p in perms]
    children.append(Elem("uses-feature", [a("name", "android.hardware.camera.any"), a("required", False)]))
    children.append(Elem("application", app_attrs, [activity]))
    root = Elem("manifest", [
        a("versionCode", version_code),
        a("versionName", version_name),
        a("package", package, android=False),
    ], children)
    return encode(root)


if __name__ == "__main__":
    out = sys.argv[1]
    icon = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    with open(out, "wb") as f:
        f.write(phonecam_manifest("com.phonecam.app", 1, "1.0", "PhoneCam", icon))
