#!/usr/bin/env python3
"""Convert a binary FBX 7.x model into the mview text model and PAM textures.

The viewer never reads FBX, zlib or PNG.  This tool runs on the build host,
reads the FBX once, and writes what the viewer needs: a text polygon list in
right-handed Y-up metres (models/README.md is the format specification),
uncompressed RGBA textures in PAM, and a provenance record.
"""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import argparse
import array
import hashlib
import io
import json
import math
import os
import pathlib
import struct
import sys
import tempfile
import zlib

from PIL import Image

# The only format version this tool writes; it is the first line of model.txt.
MODEL_FORMAT_VERSION = 1

# Textures larger than this on their longer side are shrunk to it.
DEFAULT_MAX_TEXTURE_SIDE = 1024

# Alpha classification.  A texel counts as "nearly transparent" at or below
# the low bound and "nearly opaque" at or above the high bound.  A texture
# whose texels are almost all in one of the two groups is a cut-out mask.
CUTOUT_LOW_ALPHA = 8
CUTOUT_HIGH_ALPHA = 247
CUTOUT_FRACTION = 0.98

# Material names containing one of these words are drawn without back-face
# culling: hair strands, eyes and the face decals of anime-style models are
# single-sided cards that must be seen from both sides.
DOUBLE_SIDED_WORDS = ('hair', 'eye', 'facebrow', 'faceeyeline', 'facemouth')

# Texture file names that mark a normal map; they are never exported.
NORMAL_MAP_WORDS = ('_nml', 'normal')

# The copyright statement recorded in provenance.json.
COPYRIGHT_HOLDER = 'the zedBSD project author (user); commit permitted'

# The magic at the start of every binary FBX file.
FBX_MAGIC = b'Kaydara FBX Binary  \x00'

# The first top-level record follows the magic, two reserved bytes and the version.
FBX_FIRST_RECORD = 27

# Element type codes of FBX array properties and their array module codes.
FBX_ARRAY_TYPES = {'f': 'f', 'd': 'd', 'l': 'q', 'i': 'i', 'b': 'b'}

# Element type codes of FBX scalar properties and their struct formats.
FBX_SCALAR_TYPES = {'C': '<?', 'Y': '<h', 'I': '<i', 'L': '<q', 'F': '<f', 'D': '<d'}

# Euler axis sequences of the FBX RotationOrder enum (0 = XYZ ... 5 = ZYX).
FBX_ROTATION_ORDERS = ((0, 1, 2), (0, 2, 1), (1, 2, 0), (1, 0, 2), (2, 0, 1), (2, 1, 0))


class FbxNode:
    """One record of the FBX node tree: a name, its property values, children.

    Array properties are decoded into Python lists.  String properties stay
    bytes because FBX object names carry a binary class separator.
    """

    def __init__(self, name, properties, children):
        self.name = name
        self.properties = properties
        self.children = children

    def child(self, name):
        """Return the first child called name, or None."""
        for node in self.children:
            if node.name == name:
                return node
        return None

    def children_named(self, name):
        """Return every child called name, in file order."""
        return [node for node in self.children if node.name == name]


class FbxReader:
    """Decode the binary FBX record tree from the bytes of one file."""

    def __init__(self, data):
        if not data.startswith(FBX_MAGIC):
            raise ValueError('not a binary FBX file')

        # The version follows the magic and two reserved bytes.
        self.data = data
        self.version = struct.unpack_from('<I', data, 23)[0]
        if self.version < 7000 or self.version >= 8000:
            raise ValueError(f'unsupported FBX version {self.version}')

        # Version 7.5 widened the three record header fields to 64 bits.
        self.wide = self.version >= 7500

    def read_tree(self):
        """Return the top-level nodes, stopping at the terminating null record."""
        nodes = []
        offset = FBX_FIRST_RECORD
        while True:
            node, offset = self.read_record(offset)
            if node is None:
                return nodes
            nodes.append(node)

    def read_record(self, offset):
        """Decode the record at offset; return (node or None, next offset)."""
        # The header: end offset, property count, property bytes, name length.
        if self.wide:
            end, count, _ = struct.unpack_from('<QQQ', self.data, offset)
            offset += 24
        else:
            end, count, _ = struct.unpack_from('<III', self.data, offset)
            offset += 12
        name_length = self.data[offset]
        offset += 1

        # An all-zero header closes a list of records.
        if end == 0:
            return None, offset

        name = self.data[offset:offset + name_length].decode('ascii')
        offset += name_length

        # The typed property values.
        properties = []
        for _ in range(count):
            value, offset = self.read_property(offset)
            properties.append(value)

        # Nested records fill the rest of the record up to its end offset.
        children = []
        while offset < end:
            node, offset = self.read_record(offset)
            if node is None:
                break
            children.append(node)
        if offset != end:
            raise ValueError(f'record {name} ends at {offset}, its header says {end}')
        return FbxNode(name, properties, children), end

    def read_property(self, offset):
        """Decode one typed property value; return (value, next offset)."""
        code = chr(self.data[offset])
        offset += 1

        # A scalar is stored inline.
        if code in FBX_SCALAR_TYPES:
            layout = FBX_SCALAR_TYPES[code]
            value = struct.unpack_from(layout, self.data, offset)[0]
            return value, offset + struct.calcsize(layout)

        # A string or raw blob is a length and its bytes.
        if code in ('S', 'R'):
            length = struct.unpack_from('<I', self.data, offset)[0]
            offset += 4
            return bytes(self.data[offset:offset + length]), offset + length

        # An array is an element count, an encoding (0 plain, 1 zlib) and the stored bytes.
        if code in FBX_ARRAY_TYPES:
            length, encoding, stored = struct.unpack_from('<III', self.data, offset)
            offset += 12
            payload = self.data[offset:offset + stored]
            if encoding == 1:
                payload = zlib.decompress(payload)
            elif encoding != 0:
                raise ValueError(f'unknown FBX array encoding {encoding}')
            values = array.array(FBX_ARRAY_TYPES[code])
            values.frombytes(payload)
            if sys.byteorder != 'little':
                values.byteswap()
            if len(values) != length:
                raise ValueError('FBX array length does not match its header')
            return list(values), offset + stored

        raise ValueError(f'unknown FBX property type {code!r} at offset {offset - 1}')


def object_name(node):
    """Return the readable name of an object node (before the class separator)."""
    return node.properties[1].split(b'\x00\x01')[0].decode('utf-8', 'replace')


def property_table(node):
    """Return the Properties70 of node as {name: [values...]}."""
    table = {}
    if node is None:
        return table
    block = node.child('Properties70')
    if block is None:
        return table
    for entry in block.children_named('P'):
        name = entry.properties[0].decode('utf-8', 'replace')
        table[name] = entry.properties[4:]
    return table


def output_name(text):
    """Make a name safe for a whitespace-separated line.

    Anything other than ASCII letters, digits, '.', '-' and '_' becomes '_';
    runs of '_' collapse to one, and leading and trailing '_' are removed.
    """
    characters = []
    for character in text:
        if character.isascii() and (character.isalnum() or character in '.-_'):
            characters.append(character)
        else:
            characters.append('_')
    name = ''.join(characters)
    while '__' in name:
        name = name.replace('__', '_')
    name = name.strip('_')
    if not name:
        return 'unnamed'
    return name


def fixed(value, digits):
    """Format value with a fixed number of decimals, never as negative zero."""
    text = f'{value:.{digits}f}'
    if text.startswith('-') and text.strip('-0.') == '':
        text = text[1:]
    return text


# ---------------------------------------------------------------------------
# 4x4 matrices in the column-vector convention, stored as four rows.

def identity():
    """The 4x4 identity matrix."""
    return [[1.0 if row == column else 0.0 for column in range(4)] for row in range(4)]


def multiply(left, right):
    """left * right; right is applied to a point first."""
    return [[sum(left[row][k] * right[k][column] for k in range(4))
             for column in range(4)] for row in range(4)]


def translation(vector):
    """A translation by vector."""
    matrix = identity()
    for axis in range(3):
        matrix[axis][3] = vector[axis]
    return matrix


def scaling(vector):
    """A scale by vector along the three axes."""
    matrix = identity()
    for axis in range(3):
        matrix[axis][axis] = vector[axis]
    return matrix


def axis_rotation(axis, degrees):
    """A right-handed rotation about one coordinate axis."""
    radians = math.radians(degrees)
    cosine = math.cos(radians)
    sine = math.sin(radians)
    first = (axis + 1) % 3
    second = (axis + 2) % 3
    matrix = identity()
    matrix[first][first] = cosine
    matrix[first][second] = -sine
    matrix[second][first] = sine
    matrix[second][second] = cosine
    return matrix


def euler_rotation(degrees, order):
    """An FBX Euler rotation.

    The first axis named by the order is applied first, so the default
    order XYZ is Rz * Ry * Rx.
    """
    matrix = identity()
    for axis in FBX_ROTATION_ORDERS[order]:
        matrix = multiply(axis_rotation(axis, degrees[axis]), matrix)
    return matrix


def determinant_3x3(m):
    """Determinant of a 3x3 matrix; negative means the transform mirrors."""
    return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
            - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))


def inverse_3x3(m):
    """Inverse of a 3x3 matrix: the transposed cofactors over the determinant."""
    determinant = determinant_3x3(m)
    if abs(determinant) < 1e-30:
        raise ValueError('singular transform')
    cofactor = [[m[(r + 1) % 3][(c + 1) % 3] * m[(r + 2) % 3][(c + 2) % 3]
                 - m[(r + 1) % 3][(c + 2) % 3] * m[(r + 2) % 3][(c + 1) % 3]
                 for c in range(3)] for r in range(3)]
    return [[cofactor[c][r] / determinant for c in range(3)] for r in range(3)]


def inverse_affine(matrix):
    """Inverse of an affine 4x4 matrix (linear part plus translation)."""
    linear = [row[:3] for row in matrix[:3]]
    inverse = inverse_3x3(linear)
    result = identity()
    for row in range(3):
        for column in range(3):
            result[row][column] = inverse[row][column]
        result[row][3] = -sum(inverse[row][k] * matrix[k][3] for k in range(3))
    return result


# ---------------------------------------------------------------------------
# Scene.

class Scene:
    """The parts of one FBX file the converter uses, indexed by object id."""

    def __init__(self, data):
        reader = FbxReader(data)
        self.version = reader.version
        top = {}
        for node in reader.read_tree():
            top[node.name] = node

        # Every object by its 64-bit id, in file order.
        self.objects = {}
        for node in top['Objects'].children:
            self.objects[node.properties[0]] = node

        # Property defaults per object type, from the Definitions templates.
        self.templates = {}
        definitions = top.get('Definitions')
        if definitions is not None:
            for object_type in definitions.children_named('ObjectType'):
                type_name = object_type.properties[0].decode('ascii')
                for template in object_type.children_named('PropertyTemplate'):
                    self.templates[type_name] = property_table(template)

        # Connections in file order: (kind, child id, parent id, property name).
        self.connections = []
        for entry in top['Connections'].children_named('C'):
            kind = entry.properties[0].decode('ascii')
            relation = ''
            if len(entry.properties) > 3:
                relation = entry.properties[3].decode('ascii')
            self.connections.append((kind, entry.properties[1], entry.properties[2], relation))

        self.settings = property_table(top.get('GlobalSettings'))

    def properties(self, node):
        """Object properties merged over the template of its type."""
        table = dict(self.templates.get(node.name, {}))
        table.update(property_table(node))
        return table

    def children_of(self, parent, object_type, relation=None):
        """Objects of object_type connected to parent, in connection order."""
        result = []
        for _, child, target, name in self.connections:
            if target != parent:
                continue
            if child not in self.objects:
                continue
            if self.objects[child].name != object_type:
                continue
            if relation is not None and name != relation:
                continue
            result.append(child)
        return result

    def parent_model(self, identifier):
        """The Model this Model hangs under, or None at the scene root."""
        for kind, child, target, _ in self.connections:
            if kind != 'OO' or child != identifier:
                continue
            if target in self.objects and self.objects[target].name == 'Model':
                return target
        return None


def vector_property(table, name, default):
    """A three-component property as floats, or default when it is absent."""
    values = table.get(name)
    if not values:
        return list(default)
    return [float(value) for value in values[:3]]


def local_transform(scene, node):
    """The local matrix of one Model node, by the FBX SDK's documented formula.

    L = T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1 * Soff * Sp * S * Sp^-1

    Pre- and post-rotation count only when RotationActive is set, as in the SDK.
    """
    table = scene.properties(node)
    rotation_active = bool(table.get('RotationActive', [0])[0])
    order = int(table.get('RotationOrder', [0])[0])

    pre = [0.0, 0.0, 0.0]
    post = [0.0, 0.0, 0.0]
    if rotation_active:
        pre = vector_property(table, 'PreRotation', (0, 0, 0))
        post = vector_property(table, 'PostRotation', (0, 0, 0))
    rotation_pivot = vector_property(table, 'RotationPivot', (0, 0, 0))
    scaling_pivot = vector_property(table, 'ScalingPivot', (0, 0, 0))

    steps = [
        translation(vector_property(table, 'Lcl Translation', (0, 0, 0))),
        translation(vector_property(table, 'RotationOffset', (0, 0, 0))),
        translation(rotation_pivot),
        euler_rotation(pre, 0),
        euler_rotation(vector_property(table, 'Lcl Rotation', (0, 0, 0)), order),
        inverse_affine(euler_rotation(post, 0)),
        translation([-value for value in rotation_pivot]),
        translation(vector_property(table, 'ScalingOffset', (0, 0, 0))),
        translation(scaling_pivot),
        scaling(vector_property(table, 'Lcl Scaling', (1, 1, 1))),
        translation([-value for value in scaling_pivot]),
    ]
    matrix = identity()
    for step in steps:
        matrix = multiply(matrix, step)
    return matrix


def geometric_transform(scene, node):
    """The geometry-only offset of a Model; children do not inherit it."""
    table = scene.properties(node)
    matrix = translation(vector_property(table, 'GeometricTranslation', (0, 0, 0)))
    rotation = euler_rotation(vector_property(table, 'GeometricRotation', (0, 0, 0)), 0)
    matrix = multiply(matrix, rotation)
    matrix = multiply(matrix, scaling(vector_property(table, 'GeometricScaling', (1, 1, 1))))
    return matrix


def global_transform(scene, identifier):
    """Compose the Model's scene-space matrix along its parent chain."""
    chain = []
    while identifier is not None:
        chain.append(identifier)
        identifier = scene.parent_model(identifier)
    matrix = identity()
    for model in reversed(chain):
        matrix = multiply(matrix, local_transform(scene, scene.objects[model]))
    return matrix


def axis_conversion(scene):
    """Matrix from the file's axis system and unit to right-handed Y-up metres.

    GlobalSettings names the file axis that points right (CoordAxis), up
    (UpAxis) and toward the viewer (FrontAxis), each with a sign; they become
    +X, +Y and +Z.  UnitScaleFactor is centimetres per file unit.
    """
    settings = scene.settings
    matrix = [[0.0] * 4 for _ in range(4)]
    matrix[3][3] = 1.0
    used = set()
    for row, name in ((0, 'CoordAxis'), (1, 'UpAxis'), (2, 'FrontAxis')):
        axis = int(settings.get(name, [row])[0])
        sign = int(settings.get(name + 'Sign', [1])[0])
        if axis in used or axis not in (0, 1, 2):
            raise ValueError('GlobalSettings axes do not form a basis')
        used.add(axis)
        matrix[row][axis] = 1.0 if sign >= 0 else -1.0

    metres = float(settings.get('UnitScaleFactor', [1.0])[0]) / 100.0
    return multiply(scaling((metres, metres, metres)), matrix)


def bind_pose_matrices(scene):
    """Bind-pose global matrices by node id, gathered from every Pose object."""
    poses = {}
    for node in scene.objects.values():
        if node.name != 'Pose':
            continue
        for pose_node in node.children_named('PoseNode'):
            target = pose_node.child('Node').properties[0]
            values = pose_node.child('Matrix').properties[0]
            # FBX stores the matrix column by column.
            poses[target] = [[values[column * 4 + row] for column in range(4)]
                             for row in range(4)]
    return poses


# ---------------------------------------------------------------------------
# Mesh layers.

def layer_element(geometry, name):
    """Layer element number 0 of the given kind, or None."""
    for element in geometry.children_named(name):
        if element.properties and element.properties[0] == 0:
            return element
    return None


class LayerReader:
    """Resolve one per-corner attribute through the FBX mapping and reference modes."""

    def __init__(self, element, data_name, index_name, width):
        self.width = width
        self.mapping = element.child('MappingInformationType').properties[0].decode('ascii')
        self.reference = element.child('ReferenceInformationType').properties[0].decode('ascii')
        self.values = element.child(data_name).properties[0]
        self.indices = None
        if self.reference in ('IndexToDirect', 'Index'):
            self.indices = element.child(index_name).properties[0]
        elif self.reference != 'Direct':
            raise ValueError(f'unsupported reference mode {self.reference}')

    def lookup(self, polygon, corner, control_point):
        """The attribute of one polygon corner."""
        if self.mapping == 'ByPolygonVertex':
            slot = corner
        elif self.mapping in ('ByVertice', 'ByVertex', 'ByControlPoint'):
            slot = control_point
        elif self.mapping == 'ByPolygon':
            slot = polygon
        elif self.mapping == 'AllSame':
            slot = 0
        else:
            raise ValueError(f'unsupported mapping mode {self.mapping}')

        if self.indices is not None:
            slot = self.indices[slot]
        return self.values[slot * self.width:(slot + 1) * self.width]


class MaterialLayer:
    """Per-polygon material slot: an index into the Model's connected materials."""

    def __init__(self, element):
        self.mapping = element.child('MappingInformationType').properties[0].decode('ascii')
        self.slots = element.child('Materials').properties[0]

    def lookup(self, polygon):
        """The material slot of one polygon."""
        if self.mapping == 'AllSame':
            return self.slots[0]
        if self.mapping == 'ByPolygon':
            return self.slots[polygon]
        raise ValueError(f'unsupported material mapping {self.mapping}')


class VertexFormatter:
    """Transform one polygon corner and write it as its final 'v' line."""

    def __init__(self, geometry, matrix):
        self.positions = geometry.child('Vertices').properties[0]
        self.matrix = matrix

        normal_element = layer_element(geometry, 'LayerElementNormal')
        uv_element = layer_element(geometry, 'LayerElementUV')
        self.normals = None
        self.uvs = None
        if normal_element is not None:
            self.normals = LayerReader(normal_element, 'Normals', 'NormalsIndex', 3)
        if uv_element is not None:
            self.uvs = LayerReader(uv_element, 'UV', 'UVIndex', 2)

        # Normals follow the inverse transpose of the linear part.
        linear = [row[:3] for row in matrix[:3]]
        inverse = inverse_3x3(linear)
        self.normal_matrix = [[inverse[column][row] for column in range(3)] for row in range(3)]

        # A mirroring transform turns counter-clockwise faces clockwise.
        self.mirrored = determinant_3x3(linear) < 0.0

    def corner(self, polygon, corner, control_point):
        """Return (position, line) for one polygon corner."""
        m = self.matrix
        x, y, z = self.positions[control_point * 3:control_point * 3 + 3]
        position = [m[row][0] * x + m[row][1] * y + m[row][2] * z + m[row][3] for row in range(3)]

        # The normal, transformed and renormalised; zero when the mesh has none.
        normal = [0.0, 0.0, 0.0]
        if self.normals is not None:
            nx, ny, nz = self.normals.lookup(polygon, corner, control_point)
            n = self.normal_matrix
            normal = [n[row][0] * nx + n[row][1] * ny + n[row][2] * nz for row in range(3)]
            length = math.sqrt(normal[0] ** 2 + normal[1] ** 2 + normal[2] ** 2)
            if length > 0.0:
                normal = [value / length for value in normal]

        # Vulkan samples image row 0 at v = 0; FBX puts v = 0 at the image bottom.
        u = 0.0
        v = 0.0
        if self.uvs is not None:
            u, v = self.uvs.lookup(polygon, corner, control_point)
        v = 1.0 - v

        fields = [fixed(value, 6) for value in position]
        fields += [fixed(value, 5) for value in normal]
        fields += [fixed(u, 6), fixed(v, 6)]
        return position, 'v ' + ' '.join(fields)


def triangulate(geometry, material_ids, matrix):
    """Split every polygon of one mesh into a triangle fan.

    Returns (triangles, statistics).  A triangle is (global material index,
    [three (position, line) corners]).
    """
    polygon_indices = geometry.child('PolygonVertexIndex').properties[0]
    formatter = VertexFormatter(geometry, matrix)
    material_element = layer_element(geometry, 'LayerElementMaterial')
    materials = None
    if material_element is not None:
        materials = MaterialLayer(material_element)

    triangles = []
    polygon_count = 0
    fan_triangles = 0
    skipped_polygons = 0
    corner = 0
    pending = []
    for value in polygon_indices:
        # A negative index, stored as ~index, closes the current polygon.
        closing = value < 0
        control_point = ~value if closing else value
        pending.append((corner, control_point))
        corner += 1
        if not closing:
            continue

        polygon = polygon_count
        polygon_count += 1
        corners = pending
        pending = []
        if len(corners) < 3:
            skipped_polygons += 1
            continue
        fan_triangles += len(corners) - 2

        slot = 0
        if materials is not None:
            slot = materials.lookup(polygon)
        if slot < 0 or slot >= len(material_ids):
            raise ValueError(f'polygon {polygon} uses material slot {slot} of {len(material_ids)}')
        written = [formatter.corner(polygon, c, p) for c, p in corners]

        # Fan around the first corner; a mirrored mesh swaps two corners back.
        for index in range(1, len(corners) - 1):
            triangle = [written[0], written[index], written[index + 1]]
            if formatter.mirrored:
                triangle = [triangle[0], triangle[2], triangle[1]]
            triangles.append((material_ids[slot], triangle))

    if pending:
        raise ValueError('PolygonVertexIndex does not end with a closing index')

    statistics = {
        'control_points': len(formatter.positions) // 3,
        'polygon_corners': len(polygon_indices),
        'polygons': polygon_count,
        'skipped_polygons': skipped_polygons,
        'fan_triangles': fan_triangles,
        'mirrored': formatter.mirrored,
    }
    return triangles, statistics


def weld(triangles):
    """Order triangles by material and weld identical vertex lines into indices.

    The sort is stable, so triangles keep their FBX order within a material.
    Vertices are numbered in the order the ordered triangles first use them.
    """
    ordered = sorted(range(len(triangles)), key=lambda number: (triangles[number][0], number))
    index_of = {}
    lines = []
    positions = []
    faces = []
    degenerate = 0
    for number in ordered:
        material, corners = triangles[number]
        indices = []
        for position, line in corners:
            if line not in index_of:
                index_of[line] = len(lines)
                lines.append(line)
                positions.append(position)
            indices.append(index_of[line])
        if len(set(indices)) < 3:
            degenerate += 1
        faces.append((material, indices))
    return lines, positions, faces, degenerate


# ---------------------------------------------------------------------------
# Materials and textures.

def diffuse_video(scene, material_id):
    """(Texture id, Video id) behind a material's DiffuseColor, or (None, None)."""
    for texture_id in scene.children_of(material_id, 'Texture', 'DiffuseColor'):
        for video_id in scene.children_of(texture_id, 'Video'):
            return texture_id, video_id
    return None, None


def is_normal_map_name(name):
    """True when a texture file name marks a normal map."""
    lowered = name.lower()
    for word in NORMAL_MAP_WORDS:
        if word in lowered:
            return True
    return False


def classify_alpha(image):
    """'opaque', 'cutout' or 'blend' from the alpha histogram of an RGBA image."""
    histogram = image.getchannel('A').histogram()
    total = sum(histogram)
    if histogram[255] == total:
        return 'opaque'
    binary = sum(histogram[:CUTOUT_LOW_ALPHA + 1]) + sum(histogram[CUTOUT_HIGH_ALPHA:])
    if binary >= CUTOUT_FRACTION * total:
        return 'cutout'
    return 'blend'


def encode_pam(image):
    """Uncompressed PAM (P7, RGB_ALPHA, MAXVAL 255) of an RGBA image."""
    header = (f'P7\nWIDTH {image.width}\nHEIGHT {image.height}\nDEPTH 4\n'
              f'MAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n')
    return header.encode('ascii') + image.tobytes()


def prepare_texture(png, max_side):
    """Decode, convert to RGBA and shrink so the longer side is at most max_side."""
    image = Image.open(io.BytesIO(png))
    image.load()
    source_size = image.size
    image = image.convert('RGBA')
    longer = max(image.size)
    if longer > max_side:
        ratio = max_side / longer
        width = max(1, round(image.width * ratio))
        height = max(1, round(image.height * ratio))
        image = image.resize((width, height), Image.Resampling.LANCZOS)
    return image, source_size


def material_opacity(scene, node):
    """Opacity from an explicit Opacity, else 1 - TransparencyFactor."""
    own = property_table(node)
    if 'Opacity' in own:
        return float(own['Opacity'][0])
    table = scene.properties(node)
    return 1.0 - float(table.get('TransparencyFactor', [0.0])[0])


class TextureStore:
    """The exported diffuse textures, deduplicated by the bytes of their PNG."""

    def __init__(self, max_side):
        self.max_side = max_side
        self.by_digest = {}
        self.entries = []

    def add(self, short_name, png):
        """Return the entry for this PNG, decoding it the first time it is seen."""
        digest = hashlib.sha256(png).hexdigest()
        if digest in self.by_digest:
            return self.by_digest[digest]

        image, source_size = prepare_texture(png, self.max_side)
        index = len(self.entries)
        entry = {
            'index': index,
            'file': f'tex/{index}.pam',
            'source': short_name,
            'source_png_sha256': digest,
            'source_size': list(source_size),
            'size': [image.width, image.height],
            'alpha': classify_alpha(image),
            'image': image,
        }
        self.by_digest[digest] = entry
        self.entries.append(entry)
        return entry


def describe_material(scene, material, index, textures, warnings):
    """Build the output record of one material, exporting its diffuse texture."""
    node = scene.objects[material]
    table = scene.properties(node)
    name = object_name(node)
    colour = table.get('DiffuseColor') or table.get('Diffuse') or [1.0, 1.0, 1.0]
    opacity = material_opacity(scene, node)

    # The diffuse texture, when one is embedded and is not a normal map.
    texture = None
    texture_id, video_id = diffuse_video(scene, material)
    if video_id is not None:
        video = scene.objects[video_id]
        content = video.child('Content')
        png = b''
        if content is not None and content.properties:
            png = content.properties[0]
        file_name = video.child('RelativeFilename').properties[0].decode('utf-8', 'replace')
        short_name = file_name.replace('\\', '/').split('/')[-1]
        if not png:
            warnings.append(f'{name}: diffuse texture {short_name} is not embedded')
        elif is_normal_map_name(short_name):
            warnings.append(f'{name}: diffuse slot holds normal map {short_name}; skipped')
        else:
            texture = textures.add(short_name, png)

        # UV transforms on the texture node are not carried into the format.
        texture_properties = property_table(scene.objects[texture_id])
        for key, neutral in (('Translation', [0, 0, 0]), ('Rotation', [0, 0, 0]),
                             ('Scaling', [1, 1, 1])):
            if key in texture_properties and list(texture_properties[key][:3]) != neutral:
                warnings.append(f'{name}: texture {key} is ignored')

    # Alpha mode from the texture; any material opacity below one blends.
    alpha = 'opaque'
    if texture is not None:
        alpha = texture['alpha']
    if opacity < 1.0:
        alpha = 'blend'

    # Culling: single-sided cards by name, and anything with holes.
    lowered = name.lower()
    reasons = [word for word in DOUBLE_SIDED_WORDS if word in lowered]
    if alpha != 'opaque':
        reasons.append(alpha)

    return {
        'index': index,
        'name': output_name(name),
        'source_name': name,
        'texture': texture['index'] if texture is not None else None,
        'alpha': alpha,
        'cull': 'none' if reasons else 'back',
        'cull_none_reasons': reasons,
        'color': [float(value) for value in colour[:3]] + [opacity],
    }


# ---------------------------------------------------------------------------
# Output.

def write_atomically(path, data):
    """Replace path with data without ever exposing a partial file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix='.' + path.name + '-', dir=path.parent)
    temporary = pathlib.Path(name)
    try:
        with os.fdopen(descriptor, 'wb') as output:
            output.write(data)
        temporary.chmod(0o644)
        os.replace(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def repository_relative(path):
    """path relative to the enclosing git tree, so provenance is host-independent."""
    path = path.resolve()
    for parent in [path] + list(path.parents):
        if (parent / '.git').exists():
            return path.relative_to(parent).as_posix()
    return path.name


def collect_meshes(scene):
    """(Model id, Geometry id, material ids) of every drawable mesh, in file order."""
    meshes = []
    for identifier, node in scene.objects.items():
        if node.name != 'Model' or node.properties[2] != b'Mesh':
            continue
        geometries = []
        for geometry in scene.children_of(identifier, 'Geometry'):
            if scene.objects[geometry].properties[2] == b'Mesh':
                geometries.append(geometry)
        if not geometries:
            continue
        meshes.append((identifier, geometries[0], scene.children_of(identifier, 'Material')))
    return meshes


def convert(source, output, max_side, date):
    """Read source; write model.txt, tex/*.pam and provenance.json into output."""
    data = source.read_bytes()
    scene = Scene(data)
    conversion = axis_conversion(scene)
    poses = bind_pose_matrices(scene)
    warnings = []

    # Materials are numbered in the order the meshes first use them.
    meshes = collect_meshes(scene)
    material_order = []
    for _, _, materials in meshes:
        for material in materials:
            if material not in material_order:
                material_order.append(material)
    material_index = {}
    for index, material in enumerate(material_order):
        material_index[material] = index

    # Textures are numbered in the order the materials first use them.
    textures = TextureStore(max_side)
    material_entries = []
    for material in material_order:
        entry = describe_material(scene, material, material_index[material], textures, warnings)
        material_entries.append(entry)

    # Geometry, in the model's bind pose, converted to Y-up metres.
    mesh_entries = []
    for identifier, geometry_id, materials in meshes:
        model = scene.objects[identifier]
        model_global = global_transform(scene, identifier)
        difference = None
        if identifier in poses:
            difference = max(abs(model_global[r][c] - poses[identifier][r][c])
                             for r in range(4) for c in range(4))
        matrix = multiply(model_global, geometric_transform(scene, model))
        matrix = multiply(conversion, matrix)
        ids = [material_index[material] for material in materials]
        triangles, statistics = triangulate(scene.objects[geometry_id], ids, matrix)
        lines, positions, faces, degenerate = weld(triangles)
        statistics.update({
            'name': output_name(object_name(model)),
            'geometry': object_name(scene.objects[geometry_id]),
            'vertices': len(lines),
            'triangles': len(faces),
            'degenerate_triangles': degenerate,
            'bind_pose_difference': difference,
        })
        mesh_entries.append((statistics, lines, positions, faces))

    # Bounds over every written vertex.
    everything = [position for _, _, positions, _ in mesh_entries for position in positions]
    low = [min(position[axis] for position in everything) for axis in range(3)]
    high = [max(position[axis] for position in everything) for axis in range(3)]

    # model.txt.
    text = [f'mview {MODEL_FORMAT_VERSION}',
            'bounds ' + ' '.join(fixed(value, 6) for value in low + high)]
    for entry in textures.entries:
        text.append(f"texture {entry['index']} {entry['file']} "
                    f"{entry['size'][0]} {entry['size'][1]}")
    for entry in material_entries:
        texture = '-' if entry['texture'] is None else str(entry['texture'])
        colour = ' '.join(fixed(value, 6) for value in entry['color'])
        text.append(f"material {entry['index']} {entry['name']} texture {texture} "
                    f"alpha {entry['alpha']} cull {entry['cull']} color {colour}")
    for statistics, lines, _, faces in mesh_entries:
        text.append(f"mesh {statistics['name']} vertices {len(lines)} triangles {len(faces)}")
        text.extend(lines)
        for material, indices in faces:
            text.append(f't {material} {indices[0]} {indices[1]} {indices[2]}')
    model_text = ('\n'.join(text) + '\n').encode('ascii')

    # The textures.
    outputs = {output / 'model.txt': model_text}
    for entry in textures.entries:
        pam = encode_pam(entry['image'])
        entry['pam_sha256'] = hashlib.sha256(pam).hexdigest()
        entry['pam_bytes'] = len(pam)
        outputs[output / entry['file']] = pam

    # provenance.json, with repository-relative paths only.
    converter = pathlib.Path(__file__)
    command = ['python3', repository_relative(converter), repository_relative(source),
               repository_relative(output), '--max-texture-side', str(max_side), '--date', date]
    provenance = {
        'source': {
            'file': source.name,
            'path': repository_relative(source),
            'sha256': hashlib.sha256(data).hexdigest(),
            'fbx_version': scene.version,
        },
        'converter': {
            'path': repository_relative(converter),
            'sha256': hashlib.sha256(converter.read_bytes()).hexdigest(),
        },
        'command': ' '.join(command),
        'date': date,
        'copyright': COPYRIGHT_HOLDER,
        'format': f'mview {MODEL_FORMAT_VERSION}, userland/tests/mview/models/README.md',
        'downscale': (f'an image whose longer side exceeds {max_side} is resized so that side '
                      f'is {max_side}, keeping the aspect ratio (Pillow LANCZOS on RGBA); '
                      f'smaller images are kept at their size'),
        'uv': "v' = 1 - v: FBX has v = 0 at the image bottom, Vulkan samples row 0 at v = 0",
        'model_txt_sha256': hashlib.sha256(model_text).hexdigest(),
        'counts': {
            'meshes': len(mesh_entries),
            'vertices': sum(len(lines) for _, lines, _, _ in mesh_entries),
            'triangles': sum(len(faces) for _, _, _, faces in mesh_entries),
            'materials': len(material_entries),
            'textures': len(textures.entries),
        },
        'meshes': [],
        'textures': [],
    }
    for statistics, _, _, _ in mesh_entries:
        keys = ('name', 'geometry', 'control_points', 'polygons', 'fan_triangles',
                'vertices', 'triangles', 'mirrored')
        provenance['meshes'].append({key: statistics[key] for key in keys})
    for entry in textures.entries:
        keys = ('index', 'file', 'source', 'source_png_sha256', 'source_size', 'size',
                'alpha', 'pam_sha256')
        provenance['textures'].append({key: entry[key] for key in keys})
    outputs[output / 'provenance.json'] = (json.dumps(provenance, indent=2) + '\n').encode('ascii')

    for path, content in outputs.items():
        write_atomically(path, content)

    # Remove textures of an earlier run that this run no longer produces.
    written = {path.resolve() for path in outputs}
    for stale in sorted((output / 'tex').glob('*.pam')):
        if stale.resolve() not in written:
            stale.unlink()

    return {
        'warnings': warnings,
        'meshes': [statistics for statistics, _, _, _ in mesh_entries],
        'materials': material_entries,
        'textures': textures.entries,
        'bounds': low + high,
        'model_bytes': len(model_text),
        'texture_bytes': sum(entry['pam_bytes'] for entry in textures.entries),
    }


def print_report(report):
    """A human-readable summary on standard output."""
    for mesh in report['meshes']:
        print(f"mesh {mesh['name']}: {mesh['polygons']} polygons, "
              f"{mesh['fan_triangles']} fan triangles -> {mesh['triangles']} triangles "
              f"({mesh['degenerate_triangles']} degenerate), {mesh['control_points']} "
              f"control points -> {mesh['vertices']} vertices, mirrored {mesh['mirrored']}, "
              f"bind pose difference {mesh['bind_pose_difference']}")
    for entry in report['textures']:
        print(f"texture {entry['index']} {entry['source']} "
              f"{entry['source_size'][0]}x{entry['source_size'][1]} -> "
              f"{entry['size'][0]}x{entry['size'][1]} alpha {entry['alpha']}")
    for entry in report['materials']:
        reasons = ','.join(entry['cull_none_reasons'])
        print(f"material {entry['index']} {entry['name']} texture {entry['texture']} "
              f"alpha {entry['alpha']} cull {entry['cull']} {reasons}")
    print('bounds', ' '.join(fixed(value, 4) for value in report['bounds']))
    print(f"model.txt {report['model_bytes']} bytes, textures {report['texture_bytes']} bytes")
    for warning in report['warnings']:
        print('warning:', warning)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('source', type=pathlib.Path, help='binary FBX 7.x file')
    parser.add_argument('output', type=pathlib.Path, help='model directory to write')
    parser.add_argument('--max-texture-side', type=int, default=DEFAULT_MAX_TEXTURE_SIDE)
    parser.add_argument('--date', required=True, help='conversion date for provenance.json')
    args = parser.parse_args()
    report = convert(args.source, args.output, args.max_texture_side, args.date)
    print_report(report)


if __name__ == '__main__':
    main()
