"""Comprueba las protecciones de un .exe de Windows (x64) leyendo su cabecera PE.

    py tools/check_pe.py OpenDock.exe [--strict]

Siempre exige ASLR de alta entropía y DEP. Con --strict (lo usa el CI de las versiones,
compiladas con MSVC) exige además Control Flow Guard, CET (shadow stack) y que las DLL que
no son KnownDLLs solo se busquen en System32. Sin dependencias: solo la biblioteca estándar.
"""
import struct, sys

DLL = {0x0020: 'ASLR de alta entropía', 0x0040: 'ASLR', 0x0100: 'DEP (NX)', 0x4000: 'Control Flow Guard'}
REQUIRED = [0x0020, 0x0040, 0x0100]
STRICT = [0x4000]

def rva_to_off(sections, rva):
    for va, vsz, raw, rsz in sections:
        if va <= rva < va + max(vsz, rsz):
            return rva - va + raw
    return None

def main(path, strict):
    b = open(path, 'rb').read()
    pe = struct.unpack_from('<I', b, 0x3C)[0]
    assert b[pe:pe + 4] == b'PE\0\0', 'no es un PE'
    nsec, opt_size = struct.unpack_from('<H', b, pe + 6)[0], struct.unpack_from('<H', b, pe + 20)[0]
    opt = pe + 24
    assert struct.unpack_from('<H', b, opt)[0] == 0x20B, 'se esperaba PE32+ (x64)'
    chars = struct.unpack_from('<H', b, opt + 70)[0]
    ndirs = struct.unpack_from('<I', b, opt + 108)[0]
    dirs = [struct.unpack_from('<II', b, opt + 112 + 8 * i) for i in range(ndirs)]
    sec = opt + opt_size
    sections = [struct.unpack_from('<IIII', b, sec + 40 * i + 8)[:1] + struct.unpack_from('<I', b, sec + 40 * i + 12) +
                struct.unpack_from('<I', b, sec + 40 * i + 20) + struct.unpack_from('<I', b, sec + 40 * i + 16)
                for i in range(nsec)]
    sections = [(s[1], s[0], s[2], s[3]) for s in sections]   # (va, vsize, raw, rawsize)

    ok = True
    def report(good, name, need):
        nonlocal ok
        print(('  sí  ' if good else ('  NO  ' if need else '  --  ')) + name)
        if need and not good: ok = False

    print(path)
    for bit, name in DLL.items():
        report(bool(chars & bit), name, bit in REQUIRED or (strict and bit in STRICT))

    # CET: entrada de depuración IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS (20) con el bit CET_COMPAT
    cet = False
    if len(dirs) > 6 and dirs[6][0]:
        off = rva_to_off(sections, dirs[6][0])
        for i in range(dirs[6][1] // 28):
            _, _, _, _, typ, size, _, ptr = struct.unpack_from('<IIHHIIII', b, off + 28 * i)
            if typ == 20 and size >= 4 and struct.unpack_from('<I', b, ptr)[0] & 1:
                cet = True
    report(cet, 'CET (shadow stack)', strict)

    # DependentLoadFlags (load config, desplazamiento 0x4E en x64): 0x800 = solo System32
    dlf = 0
    if len(dirs) > 10 and dirs[10][0]:
        off = rva_to_off(sections, dirs[10][0])
        if struct.unpack_from("<I", b, off)[0] >= 0x50:
            dlf = struct.unpack_from("<H", b, off + 0x4E)[0]
    report(dlf & 0x800 != 0, 'DLL dependientes solo de System32 (DependentLoadFlags)', strict)

    # importaciones: las que no son KnownDLLs solo valen con DependentLoadFlags
    imports = []
    if dirs[1][0]:
        off = rva_to_off(sections, dirs[1][0])
        while True:
            name_rva = struct.unpack_from('<I', b, off + 12)[0]
            if not name_rva: break
            n = rva_to_off(sections, name_rva)
            imports.append(b[n:b.index(b'\0', n)].decode().lower())
            off += 20
    known = {'ntdll.dll', 'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'shell32.dll', 'ole32.dll', 'oleaut32.dll',
             'msvcrt.dll', 'combase.dll', 'shlwapi.dll', 'comdlg32.dll', 'setupapi.dll', 'imm32.dll', 'rpcrt4.dll',
             'sechost.dll', 'ws2_32.dll', 'wldap32.dll', 'normaliz.dll', 'psapi.dll', 'nsi.dll', 'clbcatq.dll'}
    risky = [d for d in imports if d not in known and not d.startswith('api-ms-')]
    report(not risky or dlf & 0x800 != 0, 'sin DLL plantables' + (f' (importa {", ".join(risky)})' if risky else ''), True)
    print('OK' if ok else 'FALTAN PROTECCIONES')
    return 0 if ok else 1

if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    sys.exit(main(args[0] if args else 'OpenDock.exe', '--strict' in sys.argv))
