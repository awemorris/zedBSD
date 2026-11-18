#version 450
// Four invocations for each triangle: invocation i covers quadrant i of the target (left to right, then top to
// bottom) with the flat value 0x1000 (i + 1).
layout(triangles, invocations = 4) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) flat out uint value;
void main() {
	vec2 low = vec2(float(gl_InvocationID & 1), float(gl_InvocationID >> 1)) - 1.0;
	for (int i = 0; i < 4; i++) {
		gl_Position = vec4(low + vec2(float(i & 1), float(i >> 1)), 0.0, 1.0);
		value = 0x1000u * uint(gl_InvocationID + 1);
		EmitVertex();
	}
	EndPrimitive();
}
