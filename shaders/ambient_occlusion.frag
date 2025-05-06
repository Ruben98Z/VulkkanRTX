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



layout ( set = 0, binding = 1 ) uniform sampler2D i_position_and_depth;
layout ( set = 0, binding = 2 ) uniform sampler2D i_normal;
layout ( set = 0, binding = 3 ) uniform sampler2D i_random_numbers;
layout (  std140, set = 0, binding = 4 ) uniform KernelSamples{
	vec4 samples[64]; 
} kernels;
 
layout(location = 0) out float out_color;

const vec2 noiseScale = vec2(1600.0 / 4.0, 900.0 / 4.0); // Based on screen size
const int kernelSize = 64;
const float radius = 0.5;
const float bias = 0.025;

void main() {

    // Get input for SSAO algorithm
    vec3 fragPos = texture(i_position_and_depth,f_uv ).xyz;
    vec3 n = normalize(texture(i_normal, f_uv).rgb);
    vec3 rand = normalize(texture(i_random_numbers, f_uv * noiseScale).xyz);

    // Create TBN change-of-basis matrix: from tangent-space to view-space
    vec3 tangent = normalize(rand - n * dot(rand,n));
    vec3 bitangent = cross(n,tangent);
    mat3 TBN = mat3(tangent,bitangent,n);

    // Iterate over the sample kernel and calculate occlusion factor
    float occlusion = 0.0;

    for(int i = 0 ; i < kernelSize; i++)
    {
        // Get sample position
        vec3 samplePos  = TBN * kernels.samples[i].xyz; //from tangent to view-space
        samplePos = fragPos + samplePos  * radius;

        // Project sample position (to sample texture) (to get position on screen/texture)
        vec4 offset = vec4(samplePos ,1.0);
        offset = per_frame_data.m_projection * offset; //from view to clip-space
        offset.xyz /= offset.w; //perspective divide
        offset.xyz = offset.xyz * 0.5 + 0.5; //Transform to range 0.0 - 1.0

        // Get sample depth
        float sampleDepth = texture(i_position_and_depth,offset.xy).z; // get depth value of kernel sample

        // Range check & accumulate
        float rangeCheck = smoothstep(0.0,1.0, radius / abs(fragPos.z - sampleDepth));
        occlusion += (sampleDepth >= samplePos .z + bias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / kernelSize);

    out_color = occlusion;
}