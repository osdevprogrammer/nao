import struct

with open('arrow_load.ani', 'rb') as f:
    data = f.read()

print('=== RIFF STRUCTURE ===')
print(f'RIFF size: {struct.unpack_from("<I", data, 4)[0]}')
print(f'Form: {data[8:12]}')

offset = 12
fno = 0
while offset < len(data) and fno < 20:
    cid = data[offset:offset+4]
    csz = struct.unpack_from('<I', data, offset+4)[0]
    print(f'Chunk: {cid} size={csz}')
    offset += 8 + csz
    if csz % 2:
       offset += 1
    fno += 1