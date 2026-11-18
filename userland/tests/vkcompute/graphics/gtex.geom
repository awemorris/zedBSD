#version 450
// A geometry shader that passes its triangle on with texel (3, 2) of the 4 x 4 image as a flat value.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) flat out uint value;
void main() {
	uvec4 t = uvec4(textureLod(tex, vec2(0.875, 0.625), 0.0) * 255.0 + 0.5);
	uint v = t.x | (t.y << 8) | (t.z << 16) | (t.w << 24);
	for (int i = 0; i < 3; i++) {
		gl_Position = gl_in[i].gl_Position;
		value = v;
		EmitVertex();
	}
	EndPrimitive();
}
