#version 450
// Each vertex reads the word before the records (0) and writes its own record (four words) at its gl_VertexIndex
// with plain stores, as the GL translation captures outputs; every triangle is a point, so nothing is drawn.
layout(set = 0, binding = 0) buffer Records { uint base; uint pad[3]; uvec4 record[]; } r;
layout(location = 0) in vec4 position;
void main() {
	uint i = uint(position.y);
	r.record[r.base + uint(gl_VertexIndex)] = uvec4(i, i * 3u + 7u, floatBitsToUint(position.y * 0.5), uint(gl_VertexIndex) ^ 0xa5a5a5a5u);
	gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
