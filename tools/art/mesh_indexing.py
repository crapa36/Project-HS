"""Lossless indexing of the complete authored 13-component vertex record."""
import struct


def index_mesh(mesh):
    """Preserve triangle order and every source float bit, including signed zero."""
    source = mesh['vertices']
    indices = mesh.get('indices', range(len(source)))
    if len(indices) % 3:
        raise ValueError('Mesh index stream must contain complete triangles')
    vertices, output, lookup = [], [], {}
    for index in indices:
        if isinstance(index, bool) or not isinstance(index, int) or not 0 <= index < len(source):
            raise ValueError('Mesh index out of range')
        vertex = source[index]
        if len(vertex) != 13:
            raise ValueError('Expected 13-component authored vertex')
        # Binary64 preserves parsed JSON/Python source precision, not only cooked
        # float32. Tuple equality alone would incorrectly merge +0.0 with -0.0.
        key = struct.pack('<13d', *vertex)
        slot = lookup.get(key)
        if slot is None:
            slot = len(vertices)
            lookup[key] = slot
            vertices.append(vertex)
        output.append(slot)
    return {**mesh, 'vertices': vertices, 'indices': output}