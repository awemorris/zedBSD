#version 450
// The same with the depth test before it (early fragment tests): a fragment the test rejects never runs.
layout(early_fragment_tests) in;
layout(push_constant) uniform Push { uint vertex; uint color; } push;
layout(set = 0, binding = 0) buffer Counts { uint count[4]; } counts;
layout(location = 0) out uint color;
void main() {
	atomicAdd(counts.count[push.color], 1u);
	color = push.color;
}
