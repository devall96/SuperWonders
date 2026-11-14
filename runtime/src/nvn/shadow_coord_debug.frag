#version 450

// Diagnostic-only replacement for Wonder's shadow receiver FS. It visualizes the
// projective coordinates emitted at location 11; it does not sample the shadow map.
layout(location = 11) in vec4 shadow_coord;
layout(location = 0) out vec4 debug_color;
layout(location = 1) out vec4 debug_color_aux;

void main() {
    vec3 projected = shadow_coord.xyz / shadow_coord.w;
    debug_color = vec4(fract(projected.x), fract(projected.y), clamp(projected.z, 0.0, 1.0), 1.0);
    debug_color_aux = debug_color;
}
