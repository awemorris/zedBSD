#version 450
// A fragment shader's buffer atomics: a count of the pixels and a bit for each column.
layout(set = 0, binding = 0) buffer Counts { uint count; uint columns[2]; } counts;
layout(location = 0) out uint color;
void main() {
	uint x = uint(gl_FragCoord.x);
	atomicAdd(counts.count, 1u);
	atomicOr(counts.columns[x / 32u], 1u << (x % 32u));
	color = 7u;
}
