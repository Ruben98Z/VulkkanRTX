#version 460

#extension GL_ARB_shader_draw_parameters : enable

layout( location = 0 ) in vec2 f_uv;


//globals
struct LightData
{
    vec4 m_light_pos;
    vec4 m_radiance;
    vec4 m_attenuattion;
    mat4 view_projection;
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

layout ( set = 0, binding = 1 ) uniform sampler2D i_ssao;
 
layout(location = 0) out float out_color;

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(i_ssao,0));
    float result = 0.0;
    for(int x = -2; x < 2 ; x++)
    {
        for(int y = -2; y < 2 ; y++)
        {
            vec2 offset = vec2(float(x),float(y)) * texelSize;
            result += texture(i_ssao, f_uv + offset).r;
        }
    }
    out_color = result / (4.0 * 4.0);

}