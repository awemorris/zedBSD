#!/usr/bin/env python3
# ws075-p001: lists everything in SPIR-V modules that the i915 executor's shader compiler does not take, without stopping
# at the first thing (the compiler itself refuses the whole module at its first refusal).  The rules copy what
# src/drivers/gpu/compiler/spirv.c (and compile.c) accept as of 2026-09-28, and the geometry stage of ws075-p007a
# (spirv-geometry.inc, 2026-10-07); each rule names the function it copies.  Shape rules
# (operand sizes, nesting depths, dynamic indices, phis) are not copied: the compiler's first refusal is compared by
# run.sh to catch a module whose only gaps are of that kind.
#
#   survey.py [--first] FILE.spv ...     one line per module: its gaps (or "ok"); --first prints only the first gap
#                                        in module order, for the comparison with the compiler's own refusal
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import subprocess
import sys

# drv_gpu_spirv_declare_decoration, drv_gpu_spirv_declare_member_decoration.
DECORATIONS = {'Location', 'Binding', 'DescriptorSet', 'BuiltIn', 'ArrayStride', 'Block', 'RelaxedPrecision', 'Flat',
               'Centroid', 'NoPerspective'}
MEMBER_DECORATIONS = {'Offset', 'BuiltIn', 'MatrixStride', 'RowMajor', 'ColMajor', 'RelaxedPrecision', 'Flat', 'Centroid',
                      'NoPerspective'}

# drv_gpu_spirv_declare_variable: module variables of these storage classes (Input and Output with a Location; an Output
# without one is a block written only through its Position builtin).
STORAGE = {'Input', 'Output', 'PushConstant', 'UniformConstant', 'Uniform'}

# drv_gpu_spirv_declare (module level): what is interpreted, and what has no execution semantics.
MODULE = {'OpEntryPoint', 'OpDecorate', 'OpMemberDecorate', 'OpTypeVoid', 'OpTypeBool', 'OpTypeSampler', 'OpTypeFunction',
          'OpTypeImage', 'OpTypeInt', 'OpTypeFloat', 'OpTypeVector', 'OpTypeMatrix', 'OpTypeSampledImage', 'OpTypeArray',
          'OpTypeStruct', 'OpTypePointer', 'OpConstant', 'OpConstantTrue', 'OpConstantFalse', 'OpConstantComposite',
          'OpVariable', 'OpExtInstImport', 'OpNop', 'OpSourceContinued', 'OpSource', 'OpSourceExtension', 'OpName',
          'OpMemberName', 'OpString', 'OpLine', 'OpNoLine', 'OpModuleProcessed', 'OpCapability', 'OpExtension',
          'OpMemoryModel', 'OpExecutionMode'}

# drv_gpu_spirv_lower_instruction: the instructions of a function body that are lowered.
BODY = {'OpReturn', 'OpUnreachable', 'OpBranch', 'OpBranchConditional', 'OpSelectionMerge', 'OpLoopMerge', 'OpKill',
        'OpPhi', 'OpFDiv', 'OpFMod', 'OpFRem', 'OpFOrdEqual', 'OpFUnordEqual', 'OpFOrdNotEqual', 'OpFUnordNotEqual',
        'OpFOrdLessThan', 'OpFUnordLessThan', 'OpFOrdGreaterThan', 'OpFUnordGreaterThan', 'OpFOrdLessThanEqual',
        'OpFUnordLessThanEqual', 'OpFOrdGreaterThanEqual', 'OpFUnordGreaterThanEqual', 'OpIEqual', 'OpINotEqual',
        'OpUGreaterThan', 'OpSGreaterThan', 'OpUGreaterThanEqual', 'OpSGreaterThanEqual', 'OpULessThan', 'OpSLessThan',
        'OpULessThanEqual', 'OpSLessThanEqual', 'OpLogicalAnd', 'OpLogicalOr', 'OpLogicalNot', 'OpLogicalEqual',
        'OpLogicalNotEqual', 'OpSelect', 'OpVariable', 'OpAccessChain', 'OpLoad', 'OpStore', 'OpFAdd', 'OpFSub', 'OpFMul',
        'OpIAdd', 'OpISub', 'OpIMul', 'OpUDiv', 'OpSDiv', 'OpUMod', 'OpSRem', 'OpSMod', 'OpShiftRightLogical',
        'OpShiftRightArithmetic', 'OpShiftLeftLogical', 'OpBitwiseOr', 'OpBitwiseXor', 'OpBitwiseAnd', 'OpSNegate', 'OpNot',
        'OpConvertFToU', 'OpConvertFToS', 'OpConvertSToF', 'OpConvertUToF', 'OpBitcast', 'OpVectorTimesScalar',
        'OpMatrixTimesScalar', 'OpVectorTimesMatrix', 'OpMatrixTimesVector', 'OpMatrixTimesMatrix', 'OpTranspose',
        'OpOuterProduct', 'OpFNegate', 'OpDot', 'OpCompositeConstruct', 'OpCompositeExtract', 'OpCompositeInsert',
        'OpCopyObject', 'OpVectorShuffle', 'OpExtInst',
        'OpImageSampleImplicitLod', 'OpImageSampleExplicitLod', 'OpLabel', 'OpFunction', 'OpFunctionEnd', 'OpNop', 'OpLine',
        'OpNoLine', 'OpDPdx', 'OpDPdy', 'OpFwidth', 'OpDPdxFine', 'OpDPdxCoarse', 'OpDPdyCoarse', 'OpFwidthCoarse',
        'OpDPdyFine', 'OpFwidthFine', 'OpImageSampleDrefImplicitLod', 'OpImageSampleDrefExplicitLod',
        'OpImageSampleProjImplicitLod', 'OpImageSampleProjExplicitLod', 'OpImageSampleProjDrefImplicitLod',
        'OpImageSampleProjDrefExplicitLod', 'OpImageFetch', 'OpImage', 'OpImageQuerySizeLod', 'OpImageQuerySize',
        'OpImageQueryLevels', 'OpEmitVertex', 'OpEndPrimitive'}

# drv_gpu_spirv_geometry_mode: the execution modes of a geometry shader (Invocations of 1 alone, OutputVertices of 1 to 256).
GEOMETRY_MODES = {'InputPoints', 'InputLines', 'InputLinesAdjacency', 'Triangles', 'InputTrianglesAdjacency',
                  'OutputPoints', 'OutputLineStrip', 'OutputTriangleStrip', 'OutputVertices', 'Invocations'}

# drv_gpu_spirv_lower_load_vertex_input: the members of gl_in a geometry shader reads.
GL_IN_MEMBERS = {'Position', 'PointSize'}

# drv_gpu_spirv_lower_sample and drv_gpu_spirv_lower_texture: the image operands of a sample that are lowered.
SAMPLE_OPERANDS = {'Bias', 'Lod', 'Grad', 'ConstOffset'}

# drv_gpu_spirv_lower_fetch: the image operands of a fetch that are lowered (Sample: of a multisampled image).
FETCH_OPERANDS = {'Lod', 'ConstOffset', 'Sample'}

# The image kinds a sample, a fetch and a query take (drv_gpu_spirv_image_type() and its callers): a sample not of a
# multisampled image; a fetch also of a texel buffer, and of a multisampled 2D image (not an array); a query of any
# 1D, 2D, 3D or cube image, multisampled or not.
SAMPLE_DIMS = {'1D', '2D', '3D', 'Cube'}
FETCH_DIMS = {'1D', '2D', '3D', 'Buffer'}

# drv_gpu_spirv_lower_extended: GLSL.std.450.
EXTENDED = {'Round', 'RoundEven', 'Trunc', 'FAbs', 'SAbs', 'FSign', 'SSign', 'Floor', 'Ceil', 'Fract', 'Radians', 'Degrees',
            'Sin', 'Cos', 'Tan', 'Pow', 'Exp', 'Log', 'Exp2', 'Log2', 'Sqrt', 'InverseSqrt', 'FMin', 'UMin', 'SMin', 'FMax',
            'UMax', 'SMax', 'FClamp', 'UClamp', 'SClamp', 'FMix', 'Step', 'SmoothStep', 'Length', 'Distance', 'Cross',
            'Normalize', 'Reflect', 'Determinant', 'MatrixInverse', 'PackHalf2x16', 'UnpackHalf2x16'}

# drv_gpu_spirv_lower_store_output and drv_gpu_spirv_geometry_output: the output builtins that are written, by stage (a
# geometry shader's gl_PointSize is refused: shaderTessellationAndGeometryPointSize is not offered).
OUTPUT_BUILTINS = {'Vertex': {'Position', 'PointSize'}, 'Geometry': {'Position', 'Layer', 'PrimitiveId'}}


def disassemble(path):
	"""Returns the module's instructions as lists of words of spirv-dis --raw-id."""
	text = subprocess.run(['spirv-dis', '--raw-id', '--no-header', '--no-color', path], capture_output=True, text=True,
	                      check=True).stdout
	instructions = []
	for line in text.splitlines():
		line = line.split(';')[0].strip()
		if line:
			instructions.append(line.split())
	return instructions


def survey(path):
	"""Returns the module's gaps, in module order, each once."""
	gaps = []

	def gap(text):
		if text not in gaps:
			gaps.append(text)

	instructions = disassemble(path)
	types = {}
	variables = {}
	member_builtins = {}
	builtins = {}
	chains = {}
	constants = {}
	locations = {}
	flats = set()
	loads = {}
	stage = None
	functions = 0
	in_body = False
	for words in instructions:
		# The result id, if any, and the opcode.
		result = None
		if len(words) > 2 and words[1] == '=':
			result = words[0]
			words = words[2:]
		opcode = words[0]
		operands = words[1:]

		# Types, for the variables' and the constants' checks.
		if opcode.startswith('OpType') and result is not None:
			types[result] = words
			if opcode in ('OpTypeInt', 'OpTypeFloat') and int(operands[0]) != 32:
				gap('%d-bit %s' % (int(operands[0]), 'integers' if opcode == 'OpTypeInt' else 'floats'))

		# The entry point's stage.
		if opcode == 'OpEntryPoint':
			stage = operands[0]
			if stage not in ('Vertex', 'Fragment', 'GLCompute', 'Geometry'):
				gap('execution model %s' % stage)

		# A geometry shader's execution modes.
		if opcode == 'OpExecutionMode' and stage == 'Geometry':
			if operands[1] not in GEOMETRY_MODES:
				gap('geometry execution mode %s' % operands[1])
			if operands[1] == 'Invocations' and int(operands[2]) != 1:
				gap('geometry invocations other than one')
			if operands[1] == 'OutputVertices' and not 1 <= int(operands[2]) <= 256:
				gap('geometry OutputVertices past 256')

		# Decorations; the builtins kept for the variables.
		if opcode == 'OpDecorate':
			if operands[1] not in DECORATIONS:
				gap('decoration %s' % operands[1])
			if operands[1] == 'BuiltIn':
				builtins[operands[0]] = operands[2]
			if operands[1] == 'Location':
				locations[operands[0]] = int(operands[2])
			if operands[1] == 'Flat':
				flats.add(operands[0])
		if opcode == 'OpMemberDecorate':
			if operands[2] not in MEMBER_DECORATIONS:
				gap('member decoration %s' % operands[2])
			if operands[2] == 'BuiltIn':
				member_builtins[(operands[0], int(operands[1]))] = operands[3]

		# Functions: one, never called.
		if opcode == 'OpFunction':
			functions += 1
			in_body = True
			if functions == 2:
				gap('function calls (more than one function)')
		if opcode == 'OpFunctionCall':
			gap('function calls (more than one function)')
		if opcode == 'OpFunctionEnd':
			in_body = False
			continue

		# Module level.
		if not in_body:
			if opcode not in MODULE:
				gap('module-level %s' % opcode)
			if opcode == 'OpConstant' and types.get(operands[0], ['?'])[0] == 'OpTypeInt':
				constants[result] = int(operands[1])
			if opcode == 'OpConstantComposite':
				# drv_gpu_spirv_declare_constant_composite: vectors, matrices, arrays and structures of constants.
				kind = types.get(operands[0], ['?'])[0]
				if kind not in ('OpTypeVector', 'OpTypeMatrix', 'OpTypeArray', 'OpTypeStruct'):
					gap('constant composite of %s' % kind[6:].lower())
			if opcode == 'OpVariable':
				storage = operands[1]
				variables[result] = (storage, operands[0])
				if len(operands) > 2:
					gap('variable initializer')
				if storage not in STORAGE:
					gap('module variable in %s' % storage)
				elif storage == 'Input' and result in builtins:
					# drv_gpu_spirv_declare_variable: a vertex shader's VertexIndex and InstanceIndex are generated inputs.
					# A fragment shader's FrontFacing, FragCoord and PointCoord are the payload's facing bit, pixel
					# position, depth and w, and the point sprite's coordinate.
					generated = stage == 'Vertex' and builtins[result] in ('VertexIndex', 'InstanceIndex')
					if stage == 'Fragment' and builtins[result] in ('FrontFacing', 'FragCoord', 'PointCoord', 'PrimitiveId'):
						generated = True
					# drv_gpu_spirv_declare_geometry_input: gl_PrimitiveIDIn is the thread's payload.
					if stage == 'Geometry' and builtins[result] == 'PrimitiveId':
						generated = True
					if not generated:
						gap('input builtin %s' % builtins[result])
				elif storage == 'Input':
					# drv_gpu_spirv_lower_load: an input is floats, or integers in a vertex shader or a Flat fragment input.
					pointee = types.get(types.get(operands[0], ['', '', ''])[2], ['?', ''])
					if pointee[0] == 'OpTypeVector':
						pointee = types.get(pointee[1], ['?'])
					if pointee[0] == 'OpTypeInt' and stage != 'Vertex' and result not in flats:
						gap('integer inputs')
				# i915_compile_store_output: a fragment shader writes colours at locations 0 to 3
				# (I915_SHADER_MAX_COLOR_OUTPUTS).
				if storage == 'Output' and stage == 'Fragment' and locations.get(result, 0) >= 4:
					gap('colour outputs past location 3')
			continue

		# A function body.
		if opcode not in BODY:
			if opcode == 'OpSwitch':
				gap('OpSwitch')
			elif opcode in ('OpFunctionCall', 'OpFunctionParameter', 'OpReturnValue'):
				gap('function calls (more than one function)')
			else:
				gap('instruction %s' % opcode)
		if opcode in ('OpEmitVertex', 'OpEndPrimitive') and stage != 'Geometry':
			gap('%s outside a geometry shader' % opcode)
		if opcode == 'OpExtInst' and operands[2] not in EXTENDED:
			gap('GLSL.std.450 %s' % operands[2])
		if opcode == 'OpLoad':
			loads[result] = operands[0]
		if opcode == 'OpImage':
			# drv_gpu_spirv_lower_image: the image of a loaded sampler names its binding.
			loads[result] = loads.get(operands[1], '')
		if opcode.startswith('OpImageSample'):
			# drv_gpu_spirv_lower_sample and _texture: 1D, 2D, 3D and cube images, arrays, of floats or integers;
			# Bias, Lod, Grad and ConstOffset (the operand mask after a Dref's reference).
			mask = 4 if 'Dref' in opcode else 3
			if len(operands) > mask and not set(operands[mask].split('|')) <= SAMPLE_OPERANDS:
				gap('texture() with operands (%s)' % operands[mask])
			sampled = types.get(loads.get(operands[1], ''), ['?', ''])
			image = types.get(sampled[1], ['?', '', '?', '0', '0', '0'])
			if image[0] == 'OpTypeImage' and (image[2] not in SAMPLE_DIMS or image[5] != '0'):
				gap('texture() of a sampler%s%s' % (image[2], 'MS' if image[5] != '0' else ''))
		if opcode in ('OpImageFetch', 'OpImageQuerySizeLod', 'OpImageQuerySize', 'OpImageQueryLevels'):
			# drv_gpu_spirv_lower_fetch and _query: what FETCH_DIMS and SAMPLE_DIMS say.
			image = types.get(loads.get(operands[1], ''), ['?', '', '?', '0', '0', '0'])
			if image[0] == 'OpTypeSampledImage':
				image = types.get(image[1], ['?', '', '?', '0', '0', '0'])
			dims = FETCH_DIMS if opcode == 'OpImageFetch' else SAMPLE_DIMS
			multisampled = image[5] != '0'
			if opcode != 'OpImageFetch' or (image[2] == '2D' and image[4] == '0'):
				multisampled = False
			if image[0] == 'OpTypeImage' and (image[2] not in dims or multisampled):
				gap('%s of a sampler%s%s' % ('texelFetch()' if opcode == 'OpImageFetch' else 'textureSize()', image[2],
				                             'MS' if image[5] != '0' else ''))
			if opcode == 'OpImageFetch' and len(operands) > 3 and not set(operands[3].split('|')) <= FETCH_OPERANDS:
				gap('texelFetch() with operands (%s)' % operands[3])
		if opcode == 'OpVariable':
			# drv_gpu_spirv_lower_variable: scalars, vectors, matrices, and arrays and structures of them.
			if len(operands) > 2:
				gap('local variable initializer')
		if opcode == 'OpAccessChain':
			chains[result] = (operands[2], operands[3:])
			# drv_gpu_spirv_lower_load_vertex_input: gl_in (an unlocated, built-in-less input of a geometry shader) is
			# read at a vertex, then a member, which must be gl_Position or gl_PointSize.
			base = operands[2]
			if (stage == 'Geometry' and base in variables and variables[base][0] == 'Input' and base not in builtins and
			    base not in locations and len(operands) > 4):
				array = types.get(types.get(variables[base][1], ['', '', ''])[2], ['?', ''])
				name = member_builtins.get((array[1], constants.get(operands[4])))
				if name is not None and name not in GL_IN_MEMBERS:
					gap('gl_in member %s' % name)
		if opcode == 'OpStore':
			base, indices = chains.get(operands[0], (operands[0], []))
			if base in variables and variables[base][0] == 'Output':
				name = builtins.get(base)
				pointer = types.get(variables[base][1])
				if name is None and pointer is not None and indices:
					name = member_builtins.get((pointer[2], constants.get(indices[0])))
				if name is not None and name not in OUTPUT_BUILTINS.get(stage, set()):
					gap('output builtin %s' % name)
	return gaps


def main():
	first = False
	paths = sys.argv[1:]
	if paths and paths[0] == '--first':
		first = True
		paths = paths[1:]
	for path in paths:
		gaps = survey(path)
		if first:
			gaps = gaps[:1]
		print('%s: %s' % (path, '; '.join(gaps) if gaps else 'ok'))


if __name__ == '__main__':
	main()
