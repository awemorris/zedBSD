#version 450
// White as the first colour, the blend factor as the second (dual source).
layout(location = 0, index = 0) out vec4 first;
layout(location = 0, index = 1) out vec4 second;
void main() {
	first = vec4(1.0);
	second = vec4(0.2, 0.4, 0.6, 1.0);
}
