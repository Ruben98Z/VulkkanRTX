#version 460

layout(triangles) in;
layout(triangle_strip, max_vertices = 30) out;

#extension GL_ARB_shader_draw_parameters : enable

layout( location = 0 ) in vec3 g_position[];

//globals
struct LightData
{
    vec4 m_light_pos;
    vec4 m_radiance;
    vec4 m_attenuattion;
    mat4 m_view_projection;
};

layout( std140, set = 0, binding = 0 ) uniform PerFrameData
{
    vec4      m_camera_pos;
    mat4      m_view;
    mat4      m_projection;
    mat4      m_view_projection;
    mat4      m_inv_view;
    mat4      m_inv_projection;
    mat4      m_inv_view_projection;
    vec4      m_clipping_planes;
    LightData m_lights[ 10 ];
    uint      m_number_of_lights;
} per_frame_data;




void main() {

    for (int i = 0; i < int(per_frame_data.m_number_of_lights); ++i) {
        mat4 light_view_proj = per_frame_data.m_lights[i].m_view_projection;

        // Set layer para esta luz
        gl_Layer = i;

        // Emitimos el tri�ngulo transformado a espacio de la luz i
        for (int j = 0; j < 3; ++j) {
            vec4 world_pos = vec4(g_position[j], 1.0);
            gl_Position = light_view_proj * world_pos;
            EmitVertex();
        }

        EndPrimitive();
    }
}