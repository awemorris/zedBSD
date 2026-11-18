#version 450
// The same colour into two targets, each blended by its own attachment's equation.
layout(location = 0) out vec4 first;
layout(location = 1) out vec4 second;
void main() {
	first = vec4(0.2, 0.4, 0.6, 1.0);
	second = vec4(0.2, 0.4, 0.6, 1.0);
}
