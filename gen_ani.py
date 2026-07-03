import struct

# Read the existing arrow.cur
with open('arrow.cur', 'rb') as f:
    cur_data = f.read()

# Parse .cur header
type_val = struct.unpack_from('<H', cur_data, 2)[0]
assert type_val == 2, 'Not a .cur file'
w = cur_data[6] if cur_data[6] else 256
h = cur_data[7] if cur_data[7] else 256
hot_x = struct.unpack_from('<H', cur_data, 10)[0]
hot_y = struct.unpack_from('<H', cur_data, 12)[0]
data_offset = struct.unpack_from('<I', cur_data, 18)[0]

# Read BMP info header
bmi = cur_data[data_offset:data_offset+40]
bpp = struct.unpack_from('<H', bmi, 14)[0]
pixel_start = data_offset + 40

# Read pixel data
if bpp == 32:
    pixels = list(cur_data[pixel_start:pixel_start + w*h*4])
elif bpp == 24:
    raw = cur_data[pixel_start:pixel_start + w*h*3]
    pixels = []
    for i in range(0, len(raw), 3):
        pixels.extend([raw[i], raw[i+1], raw[i+2], 255])
else:
    raise Exception(f'Unsupported bpp: {bpp}')

print(f'Cursor: {w}x{h}, bpp={bpp}, hotspot=({hot_x},{hot_y}), pixels={len(pixels)}')

# Create 4 animation frames with slightly different positions to simulate 'loading'
frames_data = []
durations = [9, 9, 9, 9]
offsets = [(0, 0), (3, 3), (5, 0), (0, 0)]

for frame_idx in range(4):
    ox, oy = offsets[frame_idx]
    frame_pixels = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            src_x = x - ox
            src_y = y - oy
            src_idx = (src_y * w + src_x) * 4
            dst_idx = (y * w + x) * 4
            if 0 <= src_x < w and 0 <= src_y < h:
                frame_pixels[dst_idx:dst_idx+4] = pixels[src_idx:src_idx+4]
            else:
                frame_pixels[dst_idx] = 255   # b
                frame_pixels[dst_idx+1] = 0   # g
                frame_pixels[dst_idx+2] = 255 # r
                frame_pixels[dst_idx+3] = 0   # a (transparent)
    frames_data.append(bytes(frame_pixels))

# Build ANI file
ani_data = b'ANIM'  # magic
ani_data += struct.pack('<I', 4)  # 4 frames

for i in range(4):
    ani_data += struct.pack('<I', durations[i])  # duration in ticks
    ani_data += struct.pack('<I', w)              # width
    ani_data += struct.pack('<I', h)              # height
    ani_data += struct.pack('<h', hot_x)          # hotspot_x
    ani_data += struct.pack('<h', hot_y)          # hotspot_y
    ani_data += frames_data[i]

with open('arrow_load.ani', 'wb') as f:
    f.write(ani_data)

print(f'Created arrow_load.ani: {len(ani_data)} bytes, {len(frames_data)} frames')