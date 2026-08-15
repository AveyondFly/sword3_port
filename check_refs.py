import struct, sys

PATH = sys.argv[1]

with open(PATH, 'rb') as f:
    data = f.read()

assert data[:4] == b'\x7fELF', "not ELF"
is64 = data[4] == 2
ei_data = data[5]
endian = '<' if ei_data == 1 else '>'

e_phoff = struct.unpack_from(endian + 'Q', data, 0x20)[0]
e_phentsize = struct.unpack_from(endian + 'H', data, 0x36)[0]
e_phnum = struct.unpack_from(endian + 'H', data, 0x38)[0]

loads = []
dyn = None
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    p_type = struct.unpack_from(endian + 'I', data, off)[0]
    if is64:
        p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from(endian + 'QQQQQ', data, off + 8)
    else:
        p_offset, p_vaddr, p_filesz, p_memsz = struct.unpack_from(endian + 'IIII', data, off + 4)
    if p_type == 1:
        loads.append((p_offset, p_vaddr, p_filesz))
    elif p_type == 2:
        dyn = (p_offset, p_vaddr, p_filesz)

def vaddr_to_off(vaddr):
    for o, va, fs in loads:
        if va <= vaddr < va + fs:
            return o + (vaddr - va)
    return None

doff, dvaddr, dfsz = dyn
dt = {}
entsz = 16 if is64 else 8
n = dfsz // entsz
for i in range(n):
    o = doff + i * entsz
    if is64:
        d_tag, d_val = struct.unpack_from(endian + 'qQ', data, o)
    else:
        d_tag, d_val = struct.unpack_from(endian + 'iI', data, o)
    if d_tag == 0:
        break
    dt.setdefault(d_tag, []).append(d_val)

symtab_off = vaddr_to_off(dt.get(6)[0])
strtab_off = vaddr_to_off(dt.get(5)[0])
if symtab_off is None or strtab_off is None:
    print("!! cannot resolve DT_SYMTAB/DT_STRTAB"); sys.exit(1)

symsz = 24 if is64 else 16
maxsyms = (len(data) - symtab_off) // symsz
strtab_end = len(data)

STB_GLOBAL = 1
STB_WEAK = 2
matched = []
empty_run = 0
for i in range(maxsyms):
    o = symtab_off + i * symsz
    if is64:
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(endian + 'IBBHQQ', data, o)
    else:
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(endian + 'IIIBBH', data, o)
    if st_name == 0:
        empty_run += 1
        if empty_run > 64:
            break
        continue
    empty_run = 0
    name_off = strtab_off + st_name
    if name_off < 0 or name_off >= strtab_end:
        continue
    name_end = data.index(b'\x00', name_off)
    if name_end > strtab_end:
        continue
    name = data[name_off:name_end].decode('latin1', 'replace')
    if 'apkx' in name.lower():
        bind = st_info >> 4
        if st_shndx == 0:
            kind = 'U (UNDEFINED / needed from outside)'
        else:
            kind = 'T (DEFINED / exported by this lib)'
        matched.append((name, kind))

print(f"=== APKX-related dynamic symbols in {PATH} ===")
if not matched:
    print("  (none found in .dynsym)")
for m in matched:
    print(f"  {m[1]:45s} {m[0]}")

# Also raw string scan for the C++ mangled name anywhere in the file
print("\n=== raw string presence (anywhere in file) ===")
for probe in [b'GetResourceAPKX', b'_Z15GetResourceAPKXPKc', b'GetAPKXFileLen', b'_Z14GetAPKXFileLenPKc',
              b'GetAPKXFileOffset', b'_Z17GetAPKXFileOffsetPKc', b'GetAPKXFileLenv', b'_Z15GetAPKXFileLenvPKc']:
    print(f"  {'FOUND' if probe in data else 'absent':7s} {probe.decode('latin1')}")
