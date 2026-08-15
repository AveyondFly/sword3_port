#!/usr/bin/env python3
"""patch_libs.py — 把 Android .so 的 bionic 版本需求 'LIBC' 标记为 WEAK。

Android NDK 构建的 .so 在 .gnu.version_r 里把对 libm.so/libc.so 的版本需求记为
'LIBC'（bionic 版本节点名）。glibc 设备只有 'GLIBC_2.x'，故 ld.so 报
"version 'LIBC' not found" 并拒绝加载（"undefined symbol: free, version LIBC"）。

做法：把对应 Vernaux 的 vna_flags 置上 VER_FLG_WEAK(0x2)。glibc 对未满足的弱
版本需求不报错，符号仍按名解析（标准数学/libc 函数 ABI 兼容）。零位移、不改
.dynstr、可逆、最安全。原地修改文件。

用法：
    patch_libs.py [--verify] [--all] [--quiet] <file-or-dir> [<file-or-dir> ...]

  --verify   仅检查，不写盘；所有 LIBC 已为 WEAK 返回 0，否则非 0（供 CI/部署判断）
  --all      白名单兜底：对任意含 LIBC verneed 的 ELF 打补丁（默认只动白名单 9 个）
  --quiet    减少输出

默认（无 --all）仅对白名单内的随包 Android .so 打补丁，避免误改设备库。
仅用标准库，支持 32/64 位 ELF。
"""
import os
import sys
import struct

VER_FLG_WEAK = 0x2

# 部署期需要 LIBC->WEAK 的随包 Android .so 白名单。
# 设备提供（libGLESv2/EGL/libc/libm/libdl/libz）与 glibc 编译的
# libbionic_shim.so / liblog.so 无 LIBC verneed，不在此列、不 patch。
WHITELIST = {
    "libSWD3E.so",
    "libSDL2.so",
    "libSDL2_image.so",
    "libSDL2_mixer.so",
    "libSDL2_ttf.so",
    "libsmpeg2.so",
    "libmpg123.so",
    "libhidapi.so",
    "libc++_shared.so",
}


def _read_elf_sections(data):
    """解析 ELF 节头，返回 {section_name: sh_dict}；非 ELF 返回 None。"""
    if len(data) < 0x40 or data[:4] != b"\x7fELF":
        return None
    is64 = data[4] == 2
    if is64:
        (shoff,) = struct.unpack_from("<Q", data, 0x28)
        (shentsize,) = struct.unpack_from("<H", data, 0x3A)
        (shnum,) = struct.unpack_from("<H", data, 0x3C)
        (shstrndx,) = struct.unpack_from("<H", data, 0x3E)
    else:
        (shoff,) = struct.unpack_from("<I", data, 0x20)
        (shentsize,) = struct.unpack_from("<H", data, 0x2E)
        (shnum,) = struct.unpack_from("<H", data, 0x30)
        (shstrndx,) = struct.unpack_from("<H", data, 0x32)

    def sh(i):
        o = shoff + i * shentsize
        name, typ, flags, addr, off, size = struct.unpack_from("<IIQQQQ", data, o)[:6]
        link, info, align, entsize = struct.unpack_from("<IIQQ", data, o + 0x18)[:4]
        return dict(name=name, type=typ, off=off, size=size, link=link,
                    info=info, align=align, entsize=entsize)

    shstr = sh(shstrndx)

    def sec_name(i):
        s = shstr["off"]
        e = data.find(b"\x00", s + sh(i)["name"])
        return data[s + sh(i)["name"]:e].decode("latin-1")

    sections = {}
    for i in range(shnum):
        sections[sec_name(i)] = sh(i)
    return sections


def _iter_verneed(data, sections):
    """遍历 .gnu.version_r 的 verneed/vernaux，yield (p_vn, p_vna, vna_flags, vna_name)。"""
    verr = sections.get(".gnu.version_r")
    dynstr = sections.get(".dynstr")
    if not verr or not dynstr:
        return
    doff = dynstr["off"]
    voff = verr["off"]
    vsz = verr["size"]
    p = voff
    end = voff + vsz
    while p < end:
        vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from("<HHIII", data, p)
        ap = p + vn_aux
        for _ in range(vn_cnt):
            vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from("<IHHII", data, ap)
            name_off = doff + vna_name
            e = data.find(b"\x00", name_off)
            vname = data[name_off:e].decode("latin-1")
            yield (p, ap, vna_flags, vname)
            ap += vna_next
        if vn_next == 0:
            break
        p += vn_next


def patch_file(path, target="LIBC", quiet=False):
    """把 path 内 LIBC verneed 标 WEAK。返回 'patched' / 'already' / 'skip'。"""
    with open(path, "r+b") as f:
        data = bytearray(f.read())
    sections = _read_elf_sections(data)
    if sections is None:
        if not quiet:
            print("skip (not ELF):", path)
        return "skip"
    changed = 0
    already = 0
    for (_p_vn, ap, vna_flags, vname) in _iter_verneed(data, sections):
        if vname != target:
            continue
        if vna_flags & VER_FLG_WEAK:
            already += 1
        else:
            struct.pack_into("<H", data, ap + 4, vna_flags | VER_FLG_WEAK)
            changed += 1
    if changed == 0:
        if not quiet:
            if already:
                print("skip (already WEAK): %s" % path)
            else:
                print("skip (no '%s' verneed): %s" % (target, path))
        return "already" if already else "skip"
    with open(path, "r+b") as f:
        f.write(data)
    if not quiet:
        print("weakened %d '%s' verneed: %s" % (changed, target, path))
    return "patched"


def verify_file(path, target="LIBC", quiet=False):
    """检查 path 是否还存在未弱化的 LIBC verneed。返回 True 表示已全部 WEAK。"""
    with open(path, "rb") as f:
        data = f.read()
    sections = _read_elf_sections(data)
    if sections is None:
        if not quiet:
            print("skip (not ELF):", path)
        return True
    weak = 0
    unweak = 0
    for (_p_vn, ap, vna_flags, vname) in _iter_verneed(data, sections):
        if vname != target:
            continue
        if vna_flags & VER_FLG_WEAK:
            weak += 1
        else:
            unweak += 1
            if not quiet:
                print("  [FAIL] %s LIBC verneed NOT weak (Flags=0x%x)" % (path, vna_flags))
    if not quiet and weak:
        print("  [ok] %s: %d LIBC verneed already WEAK" % (path, weak))
    return unweak == 0


def _expand(targets, use_all):
    """展开文件/目录为待处理文件路径列表（默认按白名单过滤）。"""
    out = []
    for t in targets:
        if os.path.isdir(t):
            for root, _dirs, files in os.walk(t):
                for fn in files:
                    full = os.path.join(root, fn)
                    if use_all:
                        out.append(full)
                    elif fn in WHITELIST:
                        out.append(full)
        elif os.path.isfile(t):
            if use_all or os.path.basename(t) in WHITELIST:
                out.append(t)
            else:
                print("skip (not in whitelist, use --all): %s" % t)
        else:
            print("skip (not found): %s" % t)
    return out


def main(argv):
    args = list(argv)
    verify = False
    use_all = False
    quiet = False
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--verify":
            verify = True
        elif a == "--all":
            use_all = True
        elif a == "--quiet":
            quiet = True
        else:
            break
        i += 1
    targets = args[i:]
    if not targets:
        print("usage: patch_libs.py [--verify] [--all] [--quiet] <file-or-dir> [...]")
        return 2

    files = _expand(targets, use_all)
    if not files:
        if not quiet:
            print("no matching ELF files to process (whitelist filter; use --all to force)")
        return 0 if verify else 0

    if verify:
        ok = True
        for p in files:
            ok = verify_file(p, quiet=quiet) and ok
        if not quiet:
            print("RESULT: %s" % ("ALL WEAK" if ok else "SOME NOT WEAK"))
        return 0 if ok else 1

    patched = 0
    for p in files:
        if patch_file(p, quiet=quiet) == "patched":
            patched += 1
    if not quiet:
        print("done: %d file(s) patched" % patched)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
