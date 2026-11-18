#version 450
// An array of three uniform blocks: element x % 3 (a constant index in each branch, as Vulkan wants a dynamically uniform one).
layout(set = 0, binding = 1) uniform Block { uint v; } blocks[3];
layout(location = 0) out uint color;
void main() {
	uint i = uint(gl_FragCoord.x) % 3u;
	if (i == 0u)
		color = blocks[0].v;
	else if (i == 1u)
		color = blocks[1].v;
	else
		color = blocks[2].v;
}
