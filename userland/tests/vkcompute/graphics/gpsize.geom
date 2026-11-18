#version 450
// Five points of their own sizes from one triangle, each with the flat value 0x1000 (n + 1): sizes 2, 5, 8 and 16
// at pixels (8, 8), (24.5, 8.5), (48, 16) and (16, 48), and at (56.5, 56.5) half the size the vertex shader wrote
// for the first input vertex (1).
layout(triangles) in;
layout(points, max_vertices = 5) out;
layout(location = 0) flat out uint value;
void main() {
	gl_Position = vec4(8.0 / 32.0 - 1.0, 8.0 / 32.0 - 1.0, 0.0, 1.0);
	gl_PointSize = 2.0;
	value = 0x1000u;
	EmitVertex();
	gl_Position = vec4(24.5 / 32.0 - 1.0, 8.5 / 32.0 - 1.0, 0.0, 1.0);
	gl_PointSize = 5.0;
	value = 0x2000u;
	EmitVertex();
	gl_Position = vec4(48.0 / 32.0 - 1.0, 16.0 / 32.0 - 1.0, 0.0, 1.0);
	gl_PointSize = 8.0;
	value = 0x3000u;
	EmitVertex();
	gl_Position = vec4(16.0 / 32.0 - 1.0, 48.0 / 32.0 - 1.0, 0.0, 1.0);
	gl_PointSize = 16.0;
	value = 0x4000u;
	EmitVertex();
	gl_Position = vec4(56.5 / 32.0 - 1.0, 56.5 / 32.0 - 1.0, 0.0, 1.0);
	gl_PointSize = 0.5 * gl_in[0].gl_PointSize;
	value = 0x5000u;
	EmitVertex();
}
