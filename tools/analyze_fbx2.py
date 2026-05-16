"""
Binary FBX parser - extracts node tree structure to find UV layer names,
mesh names, and whether any wind data channels are present.
"""
import struct, sys, re
from collections import defaultdict

fbx_path = r'C:\Users\marvi\OneDrive\Documents\GitHub\mars_3d_engine\models\vegetation\European_Linden\HighPoly\European_Linden.fbx'

with open(fbx_path, 'rb') as f:
	raw = f.read()

# FBX binary format:
# Header: 27 bytes
# Then a series of nodes:
#   endOffset:    uint32 (offset to end of node from file start)
#   numProperties: uint32
#   propertyListLen: uint32
#   nameLen:      uint8
#   name:         <nameLen> bytes
#   properties:   <propertyListLen> bytes (each prop: type_code byte + data)
#   children:     more nodes until sentinel (13 zero bytes)

HEADER_SIZE = 27

def read_string(data, pos):
	"""Read a length-prefixed string (4-byte LE length)"""
	slen = struct.unpack_from('<I', data, pos)[0]
	return data[pos+4:pos+4+slen].decode('utf-8','replace'), pos+4+slen

def read_property(data, pos):
	"""Read one FBX property, return (value, new_pos)"""
	type_code = chr(data[pos])
	pos += 1
	if type_code == 'Y':  # int16
		v = struct.unpack_from('<h', data, pos)[0]; pos += 2
	elif type_code == 'C':  # bool
		v = bool(data[pos]); pos += 1
	elif type_code == 'I':  # int32
		v = struct.unpack_from('<i', data, pos)[0]; pos += 4
	elif type_code == 'F':  # float32
		v = struct.unpack_from('<f', data, pos)[0]; pos += 4
	elif type_code == 'D':  # float64
		v = struct.unpack_from('<d', data, pos)[0]; pos += 8
	elif type_code == 'L':  # int64
		v = struct.unpack_from('<q', data, pos)[0]; pos += 8
	elif type_code == 'S':  # string
		slen = struct.unpack_from('<I', data, pos)[0]; pos += 4
		v = data[pos:pos+slen].decode('utf-8','replace'); pos += slen
	elif type_code == 'R':  # raw bytes
		rlen = struct.unpack_from('<I', data, pos)[0]; pos += 4
		v = f'<raw {rlen} bytes>'; pos += rlen
	elif type_code in ('f','d','l','i','b'):  # arrays
		arr_len = struct.unpack_from('<I', data, pos)[0]; pos += 4
		encoding = struct.unpack_from('<I', data, pos)[0]; pos += 4
		comp_len = struct.unpack_from('<I', data, pos)[0]; pos += 4
		v = f'<array type={type_code} len={arr_len} enc={encoding} bytes={comp_len}>'
		pos += comp_len
	else:
		v = f'<unknown type {type_code!r}>'
		pos += 1  # best effort, will likely desync
	return v, pos

def parse_node(data, pos, end_pos, depth=0):
	"""Parse one FBX node and its children. Returns list of (name, props, children)."""
	nodes = []
	while pos < end_pos:
		if pos + 13 > len(data):
			break
		# Check for sentinel (13 zero bytes)
		if data[pos:pos+13] == b'\x00'*13:
			pos += 13
			break
		node_end = struct.unpack_from('<I', data, pos)[0]; pos += 4
		if node_end == 0:
			break
		num_props = struct.unpack_from('<I', data, pos)[0]; pos += 4
		prop_list_len = struct.unpack_from('<I', data, pos)[0]; pos += 4
		name_len = data[pos]; pos += 1
		name = data[pos:pos+name_len].decode('ascii','replace'); pos += name_len

		# Read properties
		props = []
		prop_start = pos
		for _ in range(num_props):
			if pos >= node_end:
				break
			try:
				v, pos = read_property(data, pos)
				props.append(v)
			except Exception as e:
				props.append(f'<error: {e}>')
				break
		pos = prop_start + prop_list_len  # skip to end of property block

		# Read children
		children = []
		if pos < node_end:
			children = parse_node(data, pos, node_end, depth+1)

		nodes.append((name, props, children))
		pos = node_end
	return nodes

print("Parsing binary FBX...")
root_nodes = parse_node(raw, HEADER_SIZE, len(raw))
print(f"Top-level nodes: {[n[0] for n in root_nodes]}")

def find_nodes(nodes, target_name, results=None):
	if results is None:
		results = []
	for name, props, children in nodes:
		if name == target_name:
			results.append((name, props, children))
		find_nodes(children, target_name, results)
	return results

def get_child(children, name):
	for n, p, c in children:
		if n == name:
			return (n, p, c)
	return None

# ── Extract all Geometry nodes ────────────────────────────────────────────
print("\n=== GEOMETRY MESHES ===")
geom_nodes = find_nodes(root_nodes, 'Geometry')
print(f"Found {len(geom_nodes)} Geometry nodes")

for i, (name, props, children) in enumerate(geom_nodes):
	# Name is typically: "Geometry\x00\x01Model" in the prop
	geom_name = props[1] if len(props) > 1 else '?'
	geom_type = props[2] if len(props) > 2 else '?'

	# Count layer elements
	layer_elems = [(n, p, c) for n, p, c in children if n.startswith('LayerElement')]
	uv_layers = [x for x in layer_elems if x[0] == 'LayerElementUV']
	color_layers = [x for x in layer_elems if x[0] == 'LayerElementColor']

	# Get vertex count
	verts_node = get_child(children, 'Vertices')
	vert_count = 0
	if verts_node:
		vp = verts_node[1]
		if vp and isinstance(vp[0], str) and 'array' in vp[0]:
			# e.g. '<array type=d len=1234 enc=0 bytes=9876>'
			m = re.search(r'len=(\d+)', vp[0])
			if m: vert_count = int(m.group(1)) // 3  # x,y,z per vertex

	print(f"\n  [{i}] {geom_name!r:50s} verts~={vert_count}")
	print(f"       UV layers: {len(uv_layers)}, Color layers: {len(color_layers)}")

	for uv_n, uv_p, uv_c in uv_layers:
		name_node = get_child(uv_c, 'Name')
		uv_name = name_node[1][0] if name_node and name_node[1] else '?'
		type_node = get_child(uv_c, 'MappingInformationType')
		map_type = type_node[1][0] if type_node and type_node[1] else '?'
		print(f"       UV: name={uv_name!r:30s} mapping={map_type!r}")

	for cl_n, cl_p, cl_c in color_layers:
		name_node = get_child(cl_c, 'Name')
		c_name = name_node[1][0] if name_node and name_node[1] else '?'
		print(f"       Color: name={c_name!r}")

# ── Look for Objects > Model nodes (mesh objects with names) ─────────────
print("\n\n=== MODEL OBJECTS (mesh names) ===")
objects_node = next((n for n in root_nodes if n[0] == 'Objects'), None)
if objects_node:
	model_nodes = [(n,p,c) for n,p,c in objects_node[2] if n == 'Model']
	for mn, mp, mc in model_nodes:
		model_name = mp[1] if len(mp) > 1 else '?'
		model_type = mp[2] if len(mp) > 2 else '?'
		if model_type == 'Mesh':
			print(f"  Mesh: {model_name!r}")

# ── Material names ────────────────────────────────────────────────────────
print("\n=== MATERIAL NAMES ===")
if objects_node:
	mat_nodes = [(n,p,c) for n,p,c in objects_node[2] if n == 'Material']
	for mn, mp, mc in mat_nodes:
		mat_name = mp[1] if len(mp) > 1 else '?'
		# Check ShadingModel
		shading_node = get_child(mc, 'ShadingModel')
		shading = shading_node[1][0] if shading_node and shading_node[1] else '?'
		print(f"  {mat_name!r:60s} shading={shading!r}")

print("\nDone.")
