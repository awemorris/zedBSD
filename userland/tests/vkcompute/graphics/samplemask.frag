#version 450
// 100 plus the coverage the pixel arrived with, kept only in odd columns by the sample mask the shader writes.
layout(location = 0) out uint color;
void main() {
	color = 100u + uint(gl_SampleMaskIn[0]);
	gl_SampleMask[0] = int(uint(gl_FragCoord.x) & 1u);
}
