import re, sys

fbx_path = r'C:\Users\marvi\OneDrive\Documents\GitHub\mars_3d_engine\models\vegetation\European_Linden\HighPoly\European_Linden.fbx'

with open(fbx_path, 'rb') as f:
	data = f.read()

print(f"File size: {len(data):,} bytes")
print(f"Header: {data[:27]}")

# ── 1. All LayerElement types ──────────────────────────────────────────────
print("\n=== LAYER ELEMENT TYPES ===")
layer_types = sorted(set(m.group(0).decode() for m in re.finditer(rb'LayerElement\w+', data)))
for t in layer_types:
	print(f"  {t}")

# ── 2. UV channel names ────────────────────────────────────────────────────
print("\n=== UV CHANNEL NAMES ===")
seen = set()
# Binary FBX stores strings as: 4-byte LE length + bytes (no null terminator)
# After 'Name' node inside LayerElementUV we get: NameS<4-len><string>
for match in re.finditer(rb'\x04Name', data):
	pos = match.end()
	if pos + 4 < len(data):
		slen = int.from_bytes(data[pos:pos+4], 'little')
		if 0 < slen < 128:
			name = data[pos+4:pos+4+slen].decode('ascii','ignore')
			if name not in seen and re.match(r'[A-Za-z]', name):
				seen.add(name)
				print(f"  {name!r}")

# ── 3. Material names ──────────────────────────────────────────────────────
print("\n=== MATERIAL NAMES ===")
seen_mat = set()
for match in re.finditer(rb'\x08Material\x00\x00', data):
	pos = match.end()
	# skip 8-byte node id, then name as length-prefixed string
	if pos + 12 < len(data):
		pos += 8  # skip id
		slen = int.from_bytes(data[pos:pos+4], 'little')
		if 0 < slen < 256:
			name = data[pos+4:pos+4+slen].decode('ascii','ignore')
			if name not in seen_mat:
				seen_mat.add(name)
				print(f"  {name!r}")

# ── 4. Look for wind / speedtree keywords anywhere ────────────────────────
print("\n=== WIND / SPEEDTREE KEYWORDS ===")
keywords = [b'Wind', b'wind', b'WIND', b'SpeedTree', b'speedtree',
			b'Branch', b'branch', b'Trunk', b'trunk', b'Frond',
			b'LeafCard', b'LeafMesh', b'BillBoard', b'LodBlend',
			b'stWindData', b'WindWeight', b'windweight',
			b'ColorSet', b'LayerElementColor']
for kw in keywords:
	hits = [m.start() for m in re.finditer(re.escape(kw), data)]
	if hits:
		print(f"  {kw.decode()!r:25s}: {len(hits)} occurrences")
		ctx = data[hits[0]:hits[0]+100]
		printable = ''.join(chr(b) if 32<=b<127 else '.' for b in ctx)
		print(f"    first: {printable}")

# ── 5. All unique node-name strings (length-prefixed, 4-byte LE) that look like identifiers ──
print("\n=== NODE PROPERTY NAMES (unique, length 4-40, no spaces) ===")
seen_props = set()
pos = 0
while pos < len(data) - 8:
	# Look for plausible short identifier strings embedded in the file
	# by scanning for 4-byte lengths in range [4,40] followed by ASCII word chars
	slen = int.from_bytes(data[pos:pos+4], 'little')
	if 4 <= slen <= 40:
		candidate = data[pos+4:pos+4+slen]
		if len(candidate) == slen:
			try:
				s = candidate.decode('ascii')
				if re.fullmatch(r'[A-Za-z][A-Za-z0-9_]+', s) and s not in seen_props:
					seen_props.add(s)
			except:
				pass
	pos += 1

interesting = [s for s in sorted(seen_props) if any(kw in s.lower() for kw in
	['wind','leaf','branch','trunk','frond','speedtree','color','weight','lod','bend','flutter','sway'])]
print(f"  Wind/leaf/branch related ({len(interesting)}):")
for s in interesting:
	print(f"    {s!r}")
