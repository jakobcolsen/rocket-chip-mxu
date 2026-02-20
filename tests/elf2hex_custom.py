import sys
import subprocess

def elf_to_hex(elf_file, hex_file):
    # Use objdump to get section information
    result = subprocess.run(['riscv64-unknown-elf-objdump', '-h', elf_file], capture_output=True, text=True)
    sections = []
    for line in result.stdout.split('\n'):
        if 'addr' in line:
            continue
        parts = line.split()
        if len(parts) >= 6 and parts[0].isdigit():
            name = parts[1]
            size = int(parts[2], 16)
            vma = int(parts[3], 16)
            if size > 0:
                sections.append((name, size, vma))

    with open(hex_file, 'w') as f:
        for name, size, vma in sections:
            if vma < 0x80000000 or vma >= 0x90000000:
                continue # Only DRAM
            
            f.write(f'@{vma:08X}\n')
            
            # Extract section binary
            data = subprocess.check_output(['riscv64-unknown-elf-objcopy', '-O', 'binary', '--only-section=' + name, elf_file, '/dev/stdout'])
            
            for i in range(0, len(data), 16):
                chunk = data[i:i+16]
                hex_str = ' '.join(f'{b:02X}' for b in chunk)
                f.write(hex_str + '\n')

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python elf2hex_custom.py <elf> <hex>")
        sys.exit(1)
    elf_to_hex(sys.argv[1], sys.argv[2])
