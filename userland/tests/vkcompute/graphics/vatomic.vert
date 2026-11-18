#version 450
// Each vertex counts itself and sets its own bit (its number is the attribute's y); every triangle is a point,
// so nothing is drawn.
layout(set = 0, binding = 0) buffer Count { uint count; uint seen[]; } c;
layout(location = 0) in vec4 position;
void main() {
	uint i = uint(position.y);
	atomicAdd(c.count, 1u);
	atomicOr(c.seen[i / 32u], 1u << (i % 32u));
	gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
