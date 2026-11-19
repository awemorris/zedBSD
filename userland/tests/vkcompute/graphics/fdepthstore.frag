#version 450
// A fragment shader that writes memory, with the depth test after it: each draw counts its fragments in the word its
// pushed number names, and writes that number.
layout(push_constant) uniform Push { uint vertex; uint color; } push;
layout(set = 0, binding = 0) buffer Counts { uint count[4]; } counts;
layout(location = 0) out uint color;
void main() {
	atomicAdd(counts.count[push.color], 1u);
	color = push.color;
}
