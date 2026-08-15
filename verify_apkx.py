import struct, sys

PATH = sys.argv[1] if len(sys.argv) > 1 else 'libbionic_shim.so'

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
    if p_type == 1:  # PT_LOAD
        loads.append((p_offset, p_vaddr, p_filesz))
    elif p_type == 2:  # PT_DYNAMIC
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

# robust: scan dynsym up to file end, guarded
symsz = 24 if is64 else 16
maxsyms = (len(data) - symtab_off) // symsz
strtab_end = len(data)

STB_GLOBAL = 1
STB_WEAK = 2
found = []
empty_run = 0
for i in range(maxsyms):
    o = symtab_off + i * symsz
    if is64:
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(endian + 'IBBHQQ', data, o)
    else:
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(endian + 'IIIBBH', data, o)
    if st_name == 0:
        empty_run += 1
        if empty_run > 64:  # trailing zero padding -> stop
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
        exported = (bind in (STB_GLOBAL, STB_WEAK)) and st_shndx != 0
        found.append((name, 'T' if st_shndx != 0 else 'U', 'EXPORTED' if exported else 'local/undef'))

print(f"symtab_off={symtab_off} strtab_off={strtab_off} scanned={maxsyms}")
for f in found:
    print(f"  {f[1]}  {f[0]}  [{f[2]}]")
if not found:
    print("  !! NO APKX symbols found in dynsym")
