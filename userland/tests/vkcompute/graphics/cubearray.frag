#version 450
// A cube array's face: +X of cube 1 left of the middle, +Y of cube 0 from it on, the texel's bytes packed.
layout(set = 0, binding = 0) uniform samplerCubeArray cubes;
layout(location = 0) out uint color;
void main() {
	vec4 coordinate = vec4(1.0, 0.0, 0.0, 1.0);
	if (gl_FragCoord.x >= 32.0)
		coordinate = vec4(0.0, 1.0, 0.0, 0.0);
	uvec4 t = uvec4(textureLod(cubes, coordinate, 0.0) * 255.0 + 0.5);
	color = t.x | (t.y << 8) | (t.z << 16) | (t.w << 24);
}
