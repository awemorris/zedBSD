#version 450
// textureQueryLod where a pixel spans four texels of the 4 x 4 image (a level of detail of 2), in sixteenths.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) out uint color;
void main() {
	vec2 q = textureQueryLod(tex, gl_FragCoord.xy);
	color = uint(q.y * 16.0 + 0.5) | (uint(q.x * 16.0 + 0.5) << 8);
}
