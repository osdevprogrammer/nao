"""
Create a proper Windows .ani file (RIFF/ACON format) from arrow.cur
This generates a standard .ani file that ani_loader.c can parse.
"""

import struct
import os

def read_cur(path):
    with open(path, 'rb') as f:
        data = f.read()
    type_val = struct.unpack_from('<H', data, 2)[0]
    assert type_val == 2
    w = data[6] if data[6] else 256
    h = data[7] if data[7] else 256
    hx = struct.unpack_from('<H', data, 10)[0]
    hy = struct.unpack_from('<H', data, 12)[0]
    offset = struct.unpack_from('<I', data, 18)[0]
    bpp = struct.unpack_from('<H', data[offset:], 14)[0]
    pixel_start = offset + 40
    if bpp == 32:
        pixels = bytes(data[pixel_start:pixel_start + w*h*4])
    else:
        raw = data[pixel_start:pixel_start + w*h*3]
        pixels = bytearray()
        for i in range(0, len(raw), 3):
            pixels.extend([raw[i], raw[i+1], raw[i+2], 0xFF])
        pixels = bytes(pixels)
    return w, h, hx, hy, bpp, pixels

w, h, hx, hy, bpp, base_pixels = read_cur('arrow.cur')
print(f"Source cursor: {w}x{h}, hotspot=({hx},{hy}), bpp={bpp}")

# Create 4 frames with offsets
offsets = [(0,0), (2,2), (4,0), (0,0)]
rate_jiffies = 15  # 15 jiffies = ~4 ticks = ~0.25s per frame, total ~1s

# Build RIFF/ACON file
buf = bytearray()
# RIFF header
buf.extend(b'RIFF')
buf.extend(struct.pack('<I', 0))  # size placeholder
buf.extend(b'ACON')

# Helper to add chunk
def add_chunk(id, data):
    buf.extend(id.encode('ascii'))
    buf.extend(struct.pack('<I', len(data)))
    buf.extend(data)
    # Pad to even boundary
    if len(data) % 2:
        buf.append(0)

# ANI header chunk (anih)
anih = struct.pack('<I', 36)  # cbSizeof
anih += struct.pack('<I', 4)   # cFrames
anih += struct.pack('<I', 4)   # cSteps
anih += struct.pack('<I', w)   # cx
anih += struct.pack('<I', h)   # cy
anih += struct.pack('<I', rate_jiffies)  # iRate
anih += struct.pack('<I', 0)   # iCurve
add_chunk('anih', anih)

# Optional sequence chunk (seq ) - use all frames in order
seq_data = struct.pack('<I', 0)
seq_data += struct.pack('<I', 1)
seq_data += struct.pack('<I', 2)
seq_data += struct.pack('<I', 3)
add_chunk('seq ', seq_data)

# Icon chunks - each contains a .cur file
for i, (ox, oy) in enumerate(offsets):
    # Build a .cur file for this frame
    cur_buf = bytearray()
    cur_buf.extend(struct.pack('<H', 2))        # type = cursor
    cur_buf.extend(struct.pack('<H', 1))        # count = 1
    cur_buf.extend(struct.pack('B', w))         # width
    cur_buf.extend(struct.pack('B', h))         # height
    cur_buf.extend(bytes(2))                    # reserved
    cur_buf.extend(struct.pack('<H', hx))       # hotspot x
    cur_buf.extend(struct.pack('<H', hy))       # hotspot y
    
    # Calculate data offset: 22 (cur header) + 40 (bmp header) = 62
    data_offset = 22 + 40
    
    # BMP info header
    bmp = bytearray()
    bmp.extend(struct.pack('<I', 40))           # header size
    bmp.extend(struct.pack('<I', w))            # width
    bmp.extend(struct.pack('<I', h * 2))        # height (doubled for cursors)
    bmp.extend(struct.pack('<H', 1))            # planes
    bmp.extend(struct.pack('<H', 32))           # bpp
    bmp.extend(bytes(24))                       # rest of header
    
    # Compose frame pixels with offset
    frame_pixels = bytearray()
    for y in range(h):
        for x in range(w):
            src_x = x - ox
            src_y = y - oy
            if 0 <= src_x < w and 0 <= src_y < h:
                idx = (src_y * w + src_x) * 4
                frame_pixels.extend([base_pixels[idx], base_pixels[idx+1], 
                                    base_pixels[idx+2], base_pixels[idx+3]])
            else:
                # Transparent
                frame_pixels.extend([0, 0, 0, 0])
    
    cur_buf.extend(bmp)
    cur_buf.extend(frame_pixels)
    
    # Update data offset in cur header (bytes 18-21)
    struct.pack_into('<I', cur_buf, 18, data_offset)
    
    add_chunk('icon', bytes(cur_buf))

# Fix RIFF size
total_size = len(buf) - 8
struct.pack_into('<I', buf, 4, total_size)

out_path = 'arrow_load.ani'
with open(out_path, 'wb') as f:
    f.write(buf)

print(f"Created {out_path}: {len(buf)} bytes, 4 frames at {rate_jiffies} jiffies each")
print(f"Format: RIFF/ACON with anih + seq + 4xicon chunks")