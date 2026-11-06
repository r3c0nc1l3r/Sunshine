#version 300 es

#ifdef GL_ES
precision highp float;
#endif

uniform highp sampler2D image;

layout(shared) uniform ColorMatrix {
  vec4 color_vec_y;
  vec4 color_vec_u;
  vec4 color_vec_v;
  vec2 range_y;
  vec2 range_uv;
};

in highp vec2 tex;
layout(location = 0) out float color;

void main()
{
	vec3 rgb = texture(image, tex).rgb;
	float y = dot(color_vec_y.xyz, rgb) + color_vec_y.w;

	// Source is already full-range BT.2020/PQ: apply only the encoder's NCL matrix/range.
	// P010 stores rounded 10-bit codes in bits 6..15 of each UNORM16 sample.
	color = floor(clamp(y * range_y.x + range_y.y, 0.0, 1.0) * 1023.0 + 0.5) * (64.0 / 65535.0);
}
