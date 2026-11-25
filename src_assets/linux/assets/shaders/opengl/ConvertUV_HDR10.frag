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

in highp vec3 uuv;
layout(location = 0) out vec2 color;

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
void main() {
  vec3 rgb_left  = texture(image, uuv.xz).rgb;
  vec3 rgb_right = texture(image, uuv.yz).rgb;
  vec3 rgb       = (rgb_left + rgb_right) * 0.5;

  float u = dot(color_vec_u.xyz, rgb) + color_vec_u.w;
  float v = dot(color_vec_v.xyz, rgb) + color_vec_v.w;

  u = u * range_uv.x + range_uv.y;
  v = v * range_uv.x + range_uv.y;

  // Preserve the existing chroma sampling footprint; round and left-align P010 codes.
  color = floor(clamp(vec2(u, v), 0.0, 1.0) * 1023.0 + 0.5) * (64.0 / 65535.0);
}
