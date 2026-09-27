#version 450
// nfsmw - native renderer: output pass with the game's gamma ramp and the optional postprocessing.
//
// The Xbox 360 does not output the frontbuffer as is: the display passes each 8-bit channel through the
// 256-entry, 10-bit table the game loads (DC_LUT_30_COLOR, register_table.inc). NFSMW loads a table that
// is not the identity ([64] = 273 and [128] = 539 instead of 256 and 513), and without it midtones and
// shadows came out darker.
//
// Specialization constants:
//   - 0, kExacta (the normal case: output the same size as the source): reads the pixel's texel with
//     texelFetch, unfiltered. It is cheaper than bilinear sampling and pays for the three table reads.
//     Without kExacta, it samples like the SDK's guest_output_bilinear_ps, for outputs of another size.
//   - 1, kGraduacion (postprocessing with saturation, vibrance, vignette or scanlines): the table carries
//     the game's ramp with temperature, brightness, contrast and tint, and saturation, vibrance, gamma,
//     vignette and scanlines are done here, in the order of GoldenEye-Recomp (ge_grade.cs.hlsl and
//     ge_postfx.cpp, public domain). Without kGraduacion, everything single-channel is already in the
//     table (nfsmw_nativo_destinos.cpp, RecalcularRampa) and costs nothing.
//   - 2, kFxaa (nfsmw_antialiasing = fxaa): smooths the edges of the game image before the table, with
//     the SDK's FXAA at normal quality (GetSwapFxaaComputeSource in vulkan/command_processor.cpp): five
//     luma reads and, only on edges, four color reads. It partly replaces the Xbox 360's 4x MSAA, which
//     the Switch lacks; it costs some GPU time.
// The push constants are those of guest_output_bilinear_ps: the offset from byte 16 and the inverse
// size from byte 24.
//
// The header is generated with:
//   glslangValidator -V --target-env vulkan1.0 --vn nfsmw_salida_rampa_gamma_ps -o nfsmw_salida_rampa_gamma_ps.h
//     nfsmw_salida_rampa_gamma.frag

layout(constant_id = 0) const bool kExacta = false;
layout(constant_id = 1) const bool kGraduacion = false;
layout(constant_id = 2) const bool kFxaa = false;

layout(push_constant) uniform Constantes {
  layout(offset = 16) ivec2 desplazamiento;
  layout(offset = 24) vec2 inverso;
} constantes;

layout(set = 0, binding = 0) uniform texture2D origen;
layout(set = 0, binding = 1) uniform sampler muestreador;
layout(set = 0, binding = 2, std140) uniform Rampa {
  // Red, green and blue of each entry (10-bit value / 1023, with the single-channel postprocessing).
  vec4 entradas[256];
  vec4 mezcla;   // saturacion, vibracion, 1 / gamma (kGraduacion)
  vec4 efectos;  // vineta, lineas (kGraduacion)
} rampa;

layout(location = 0) out vec4 color;

const vec3 kPesosLuma = vec3(0.299, 0.587, 0.114);

float Luma(vec2 uv) {
  return dot(textureLod(sampler2D(origen, muestreador), uv, 0.0).rgb, kPesosLuma);
}

// FXAA at normal quality, like the SDK's (edge threshold 0.166, minimum 0.0833 and an 8-texel reach).
vec3 Fxaa(vec2 uv) {
  vec2 paso = constantes.inverso;
  vec3 rgb_centro = textureLod(sampler2D(origen, muestreador), uv, 0.0).rgb;
  float luma_centro = dot(rgb_centro, kPesosLuma);
  float luma_no = Luma(uv + vec2(-paso.x, -paso.y));
  float luma_ne = Luma(uv + vec2(paso.x, -paso.y));
  float luma_so = Luma(uv + vec2(-paso.x, paso.y));
  float luma_se = Luma(uv + vec2(paso.x, paso.y));
  float luma_min = min(luma_centro, min(min(luma_no, luma_ne), min(luma_so, luma_se)));
  float luma_max = max(luma_centro, max(max(luma_no, luma_ne), max(luma_so, luma_se)));
  if (luma_max - luma_min < max(0.0833, luma_max * 0.166)) {
    return rgb_centro;
  }
  vec2 direccion = vec2(-((luma_no + luma_ne) - (luma_so + luma_se)), (luma_no + luma_so) - (luma_ne + luma_se));
  float reduccion = max((luma_no + luma_ne + luma_so + luma_se) * (0.25 * 0.125), 1.0 / 128.0);
  float inversa_min = 1.0 / (min(abs(direccion.x), abs(direccion.y)) + reduccion);
  direccion = clamp(direccion * inversa_min, vec2(-8.0), vec2(8.0)) * paso;
  vec3 rgb_a = 0.5 * (textureLod(sampler2D(origen, muestreador), uv + direccion * (1.0 / 3.0 - 0.5), 0.0).rgb +
                      textureLod(sampler2D(origen, muestreador), uv + direccion * (2.0 / 3.0 - 0.5), 0.0).rgb);
  vec3 rgb_b = rgb_a * 0.5 + 0.25 * (textureLod(sampler2D(origen, muestreador), uv + direccion * -0.5, 0.0).rgb +
                                     textureLod(sampler2D(origen, muestreador), uv + direccion * 0.5, 0.0).rgb);
  float luma_b = dot(rgb_b, kPesosLuma);
  return (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy) - constantes.desplazamiento;
  vec3 texel;
  if (kFxaa) {
    texel = Fxaa((vec2(uvec2(pixel)) + 0.5) * constantes.inverso);
  } else if (kExacta) {
    texel = texelFetch(sampler2D(origen, muestreador), pixel, 0).rgb;
  } else {
    texel = textureLod(sampler2D(origen, muestreador), (vec2(uvec2(pixel)) + 0.5) * constantes.inverso, 0.0).rgb;
  }
  uvec3 i = uvec3(texel * 255.0 + 0.5);
  vec3 c = vec3(rampa.entradas[i.r].r, rampa.entradas[i.g].g, rampa.entradas[i.b].b);
  if (kGraduacion) {
    // Saturation around luma, and vibrance (boosts the least saturated pixels more).
    float luma = dot(max(c, 0.0), vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, rampa.mezcla.x);
    float saturado = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    c = mix(vec3(luma), c, 1.0 + rampa.mezcla.y * (1.0 - clamp(saturado, 0.0, 1.0)));
    c = pow(max(c, vec3(0.0001)), vec3(rampa.mezcla.z));
    // Vignette: four bands of 42 % from each edge, dark at the edge and transparent inward, which add up
    // in the corners (like GoldenEye-Recomp's ImGui ones).
    vec2 uv = (vec2(uvec2(pixel)) + 0.5) * constantes.inverso;
    float v = rampa.efectos.x;
    c *= (1.0 - v * max(0.0, 1.0 - uv.y / 0.42)) * (1.0 - v * max(0.0, 1.0 - (1.0 - uv.y) / 0.42)) *
         (1.0 - v * max(0.0, 1.0 - uv.x / 0.42)) * (1.0 - v * max(0.0, 1.0 - (1.0 - uv.x) / 0.42));
    // Scanlines: one dark row out of every three.
    if ((uint(pixel.y) % 3u) == 0u) {
      c *= 1.0 - rampa.efectos.y * 0.7;
    }
    c = clamp(c, 0.0, 1.0);
  }
  color = vec4(c, 1.0);
}
