#version 450
// The colour the draw pushes (the word after the vertex stage's one).
layout(push_constant) uniform Push { uint vertex; uint color; } push;
layout(location = 0) out uint color;
void main() { color = push.color; }
