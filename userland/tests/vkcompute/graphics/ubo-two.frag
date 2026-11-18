#version 450
// Two uniform blocks, each a word: the descriptors a write and a copy put there.
layout(set = 0, binding = 1) uniform First { uint v; } first;
layout(set = 0, binding = 2) uniform Second { uint v; } second;
layout(location = 0) out uint color;
void main() { color = first.v ^ (second.v << 1); }
