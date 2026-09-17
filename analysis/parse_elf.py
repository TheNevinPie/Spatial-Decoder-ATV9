import struct
import sys

def read_elf(filepath):
    with open(filepath, 'rb') as f:
        data = f.read()
    
    # Check ELF magic
    if data[:4] != b'\x7fELF':
        print(f"Not an ELF file: {filepath}")
        return
    
    # Parse ELF header
    ei_class = data[4]
    ei_data = data[5]
    
    if ei_class == 1:
        # 32-bit ELF header is 52 bytes total
        # e_ident[16] + e_type[2] + e_machine[2] + e_version[4] + e_entry[4] + e_phoff[4] + e_shoff[4] + e_flags[4] + e_ehsize[2] + e_phentsize[2] + e_phnum[2] + e_shentsize[2] + e_shnum[2] + e_shstrndx[2] = 52
        if len(data) < 52:
            print("File too small")
            return
        vals = struct.unpack('<HHIIIIIHHHHHH', data[16:52])
        e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = vals
        print(f"32-bit ELF")
        print(f"  Type: {e_type}, Machine: {e_machine}, Version: {e_version}")
        print(f"  Entry: 0x{e_entry:x}")
        print(f"  Program headers: {e_phnum} at offset {e_phoff} (size {e_phentsize})")
        print(f"  Section headers: {e_shnum} at offset {e_shoff} (size {e_shentsize})")
        
        # Read section headers
        for i in range(e_shnum):
            sh_offset = e_shoff + i * e_shentsize
            if sh_offset + 40 > len(data):
                break
            vals = struct.unpack('<IIIIIIIIII', data[sh_offset:sh_offset+40])
            sh_name, sh_type, sh_flags, sh_addr, sh_offset_sec, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = vals
            print(f"  Section {i}: name_idx={sh_name}, type={sh_type}, offset={sh_offset_sec}, size={sh_size}, flags={sh_flags:x}")
    elif ei_class == 2:
        print("64-bit ELF (not fully parsed)")
    else:
        print(f"Unknown ELF class: {ei_class}")

def extract_dynamic_section(filepath):
    with open(filepath, 'rb') as f:
        data = f.read()
    
    if data[:4] != b'\x7fELF':
        print(f"Not an ELF file: {filepath}")
        return
    
    ei_class = data[4]
    if ei_class != 1:
        print("Only 32-bit ELF supported")
        return
    
    # Parse ELF header to get program header info
    vals = struct.unpack('<HHIIIIIHHHHHH', data[16:52])
    e_phoff, e_phentsize, e_phnum = vals[4], vals[8], vals[9]
    
    # Find dynamic segment
    for i in range(e_phnum):
        ph_offset = e_phoff + i * e_phentsize
        if ph_offset + 32 > len(data):
            break
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack('<IIIIIIII', data[ph_offset:ph_offset+32])
        if p_type == 2:  # PT_DYNAMIC
            print(f"Dynamic segment at file offset {p_offset}, size {p_filesz}")
            # Parse dynamic entries
            dyn_offset = p_offset
            while dyn_offset + 8 <= p_offset + p_filesz:
                d_tag, d_val = struct.unpack('<II', data[dyn_offset:dyn_offset+8])
                if d_tag == 0:  # DT_NULL
                    break
                elif d_tag == 1:  # DT_NEEDED
                    print(f"  NEEDED: index {d_val}")
                dyn_offset += 8
            break

def extract_strings(filepath, min_len=4):
    with open(filepath, 'rb') as f:
        data = f.read()
    
    result = []
    current = []
    for b in data:
        if 32 <= b <= 126:
            current.append(chr(b))
        else:
            if len(current) >= min_len:
                result.append(''.join(current))
            current = []
    return result

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <elf-file>", file=sys.stderr)
        sys.exit(2)
    target = sys.argv[1]
    print(f"=== Analyzing {target} ===")
    read_elf(target)
    print()
    extract_dynamic_section(target)
    print()
    print("=== Relevant Strings ===")
    strings = extract_strings(target, 4)
    for s in strings:
        if any(kw in s.lower() for kw in ['ac3', 'eac3', 'dts', 'truehd', 'ffmpeg', 'avcodec', 'decode', 'init', 'create', 'omx', 'stagefright', 'soft']):
            print(f"  {s}")