#version 460


#extension GL_ARB_shader_draw_parameters : enable
#extension GL_EXT_ray_query : enable

#define INV_PI 0.31830988618
#define PI   3.14159265358979323846264338327950288

layout( location = 0 ) in vec2 f_uvs;

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

layout ( set = 0, binding = 1 ) uniform sampler2D i_albedo;
layout ( set = 0, binding = 2 ) uniform sampler2D i_position_and_depth;
layout ( set = 0, binding = 3 ) uniform sampler2D i_normal;
layout ( set = 0, binding = 4 ) uniform sampler2D i_material;
layout ( set = 0, binding = 5 ) uniform sampler2D i_ssao;
layout ( set = 0, binding = 6 ) uniform sampler2DArray i_shadow_map;
layout ( set = 0, binding = 7 ) uniform accelerationStructureEXT tlas;

 
layout(location = 0) out vec4 out_color;




vec3 sampleDirectionInCone(vec3 coneDirection, float coneAngle, uint seed) {
    // Método de muestreo uniforme en el cono
    float u1 = fract(sin(dot(vec2(seed, seed + 1), vec2(12.9898, 78.233))) * 43758.5453);
    float u2 = fract(sin(dot(vec2(seed + 2, seed + 3), vec2(39.3468, 11.1357))) * 24634.6345);

    float cosTheta = mix(cos(coneAngle), 1.0, pow(u1, 4.0));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float phi = 2.0 * 3.141592 * u2;

    vec3 direction;
    direction.x = cos(phi) * sinTheta;
    direction.y = sin(phi) * sinTheta;
    direction.z = cosTheta;

    // Crear base ortonormal para transformar la dirección
    vec3 up = abs(coneDirection.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, coneDirection));
    vec3 bitangent = cross(coneDirection, tangent);

    return normalize(tangent * direction.x + bitangent * direction.y + coneDirection * direction.z);
}


float evalVisibilityRTXSoft(vec3 frag_pos, vec3 normal, vec3 light_dir, float coneAngle, int numSamples) {
    vec3 origin = frag_pos + normal * 0.01;
    float t_min = 0.001;
    float t_max = 100.0;

    int visibleCount = 0;
    uint randSeed = uint(gl_FragCoord.x * 17.0 + gl_FragCoord.y * 131.0);

    for (int i = 0; i < numSamples; ++i) {
        vec3 sample_dir = sampleDirectionInCone(light_dir, coneAngle, randSeed + uint(i));

        rayQueryEXT ray_query;
        rayQueryInitializeEXT(
            ray_query,
            tlas,
            gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
            0xFF,
            origin,
            t_min,
            sample_dir,
            t_max
        );

        while(rayQueryProceedEXT(ray_query)) {
            if(rayQueryGetIntersectionTypeEXT(ray_query, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
                rayQueryConfirmIntersectionEXT(ray_query);
            }
        }

        if (rayQueryGetIntersectionTypeEXT(ray_query, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
            visibleCount += 1;
        }
    }

    float rawVisibility = float(visibleCount) / float(numSamples);
    return clamp((rawVisibility - 0.2) / 0.8, 0.0, 1.0); // Ajuste de umbral
}

float evalVisibilityRTX(vec3 frag_pos, vec3 normal, vec3 light_dir) {
    vec3 origin = frag_pos + normal * 0.01;
    vec3 direction = normalize(light_dir);
    float t_min = 0.001;
    float t_max = 100.0;

    rayQueryEXT ray_query;

    rayQueryInitializeEXT(
        ray_query,
        tlas,
        gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
        0xFF,
        origin,
        t_min,
        direction,
        t_max
    );

    bool hit = false;
    while(rayQueryProceedEXT(ray_query)) {
        if(rayQueryGetIntersectionTypeEXT(ray_query, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            rayQueryConfirmIntersectionEXT(ray_query);
        }
    }

    if (rayQueryGetIntersectionTypeEXT(ray_query, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
        hit = true;
    }

    return hit ? 0.0 : 1.0;
}




float evalVisibilityShadowMapping(vec4 fragPosLightSpace, vec3 normal, vec3 lightDir, uint lightIndex) {
    // 1. Proyección a coordenadas de la luz
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    
    // 2. Verificar si está fuera del frustum de la luz
    if(projCoords.z < -1.0 || projCoords.z > 1.0)
        return 1.0; // Fuera del área de influencia: considerado visible

    // 3. Convertir a coordenadas de textura [0,1]
     projCoords.xy  = projCoords.xy * 0.5 + 0.5;
    
    // 4. Early exit para coordenadas fuera del shadow map
    if(projCoords.x < 0.0 || projCoords.x > 1.0 || 
       projCoords.y < 0.0 || projCoords.y > 1.0)
        return 1.0;
    
    // 5. Ajustar para depth range de la luz
    float currentDepth = projCoords.z;
    
    // 6. Muestreo del shadow map con offset suavizado
    float shadowMapDepth = texture(i_shadow_map, vec3(projCoords.xy, float(lightIndex))).r;

    // 7. Comparación con tolerancia para precisión de depth
    float depthDifference = shadowMapDepth - currentDepth;
    return (depthDifference < -0.0001) ? 0.0 : 1.0;
}




vec3 evalDiffuse()
{
    vec4  albedo       = texture( i_albedo  , f_uvs );
    vec3  n            = normalize( texture( i_normal, f_uvs ).rgb * 2.0 - 1.0 );    
    vec3  frag_pos     = texture( i_position_and_depth, f_uvs ).xyz;
    vec3  shading = vec3( 0.0 );


    for( uint id_light = 0; id_light < per_frame_data.m_number_of_lights; id_light++ )
    {
        LightData light = per_frame_data.m_lights[ id_light ];
        uint light_type = uint( floor( light.m_light_pos.a ) );

        vec4 light_space_pos = light.view_projection * vec4(frag_pos, 1.0);
        

        switch( light_type )
        {
            case 0: //directional
            {
                vec3 l = normalize( - light.m_light_pos.xyz );
				//float visibility = evalVisibilityShadowMapping( light_space_pos, n, l, id_light);

                float coneAngle = 0.05; // Ajusta el ángulo según necesidad
                int numSamples = 32;    // Muestras por pixel
                //float visibility = evalVisibilityRTXSoft(frag_pos, n, l, coneAngle, numSamples);

                float visibility = evalVisibilityRTX(frag_pos, n, l);

                float softVisibility = max(visibility, 0.025);

                shading += max( dot( n, l ), 0.0 ) * light.m_radiance.rgb * albedo.rgb * softVisibility;
                break;
            }
            case 1: //point
            {
                vec3 l = light.m_light_pos.xyz - frag_pos;
                float dist = length( l );
                float att = 1.0 / (light.m_attenuattion.x + light.m_attenuattion.y * dist + light.m_attenuattion.z * dist * dist );
                vec3 radiance = light.m_radiance.rgb * att;
                l = normalize(l);

				//float visibility = evalVisibilityShadowMapping( light_space_pos, n, l, id_light);

                float lightRadius = 0.25;
                float coneAngle = atan(lightRadius / dist);
                int numSamples = 32;
                float visibility = evalVisibilityRTXSoft(frag_pos, n, l, coneAngle, numSamples);

                //float visibility = evalVisibilityRTX(frag_pos, n, l);

                float softVisibility = max(visibility, 0.025);

                shading += max( dot( n, l ), 0.0 ) * albedo.rgb * radiance * softVisibility;
                break;
            }
            case 2: //ambient
            {
                shading += light.m_radiance.rgb * albedo.rgb ;
                break;
            }
        }
    }

    return shading;
}

// Trowbridge-Reitz GGX Normal Distribution
float D_GGX(float NH, float roughness) {
    float roughSq = roughness * roughness;
    roughSq *= roughSq;
    float denominator = NH * NH * (roughSq - 1.0) + 1.0;
    return roughSq / (max(denominator * denominator * PI, 0.000001));
}

float G_Schlick(float NV, float k) {
    return NV / (NV * (1.0 - k) + k);
}

float G_Smith(float NV, float NL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return G_Schlick(NV, k) * G_Schlick(NL, k);
}

vec3 F_Schlick(float HV, vec3 F0) {
     return F0 + (1.0 - F0) * pow(1.0 - HV, 5.0);
}

vec3 evalMicrofacets() {
    vec3 surfaceColor = vec3(0.0);
    
    vec4 baseColor = texture(i_albedo, f_uvs);
    vec3 surfaceNormal = normalize(texture(i_normal, f_uvs).rgb * 2.0 - 1.0);
    vec3 fragPosition = texture(i_position_and_depth, f_uvs).xyz;
    vec4 material = texture(i_material, f_uvs);

    for(uint id_light = 0; id_light < per_frame_data.m_number_of_lights; id_light++) {
        LightData light = per_frame_data.m_lights[ id_light ];
        uint light_type = uint( floor( light.m_light_pos.a ) );

        vec3 lightDir;
        vec3 viewDir = normalize(per_frame_data.m_camera_pos.xyz - fragPosition);

        vec4 light_space_pos = light.view_projection * vec4(fragPosition, 1.0);
        

        if(light_type == 0) { // Direccionales
            lightDir = normalize(light.m_light_pos.xyz);
        } 
        else if(light_type == 1) { // Puntuales
            lightDir = normalize(light.m_light_pos.xyz - fragPosition);
        }
        

        // Cálculos comunes
        vec3 halfwayVec = normalize(viewDir + lightDir);
        float NdotV = max(dot(surfaceNormal, viewDir), 1e-7);
        float NdotL = max(dot(surfaceNormal, lightDir), 1e-7);
        float NdotH = max(dot(surfaceNormal, halfwayVec), 0.0);
        float HdotV = max(dot(halfwayVec, viewDir), 0.0);

        // Parámetros PBR
        float roughness = material.z;
        vec3 F0 = mix(vec3(0.04), baseColor.rgb, material.y);
        vec3 F = F_Schlick(HdotV, F0);
        vec3 kD = (vec3(1.0) - F) * (1.0 - material.y); 

        // BRDF
        float D = D_GGX(NdotH, roughness);
        float G = G_Smith(NdotV, NdotL, roughness);
        vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);

        // Componentes
        vec3 diffuse = kD * baseColor.rgb * INV_PI;
        vec3 Lo = vec3(0.0);

        if(light_type == 0){
			//float visibility = evalVisibilityShadowMapping( light_space_pos, surfaceNormal, lightDir, id_light);
            
            float coneAngle = 0.05;
            int numSamples = 32;
            //float visibility = evalVisibilityRTXSoft(fragPosition, surfaceNormal, lightDir, coneAngle, numSamples);

            float visibility = evalVisibilityRTX(fragPosition, surfaceNormal, lightDir);

            float softVisibility = max(visibility, 0.025);

            Lo = (diffuse + specular) * light.m_radiance.rgb * NdotL * softVisibility;
        } 
        else if(light_type == 1){
            vec3 toLight = light.m_light_pos.xyz - fragPosition;
            float dist = length(toLight);
            float attenuation = 1.0 / (light.m_attenuattion.x + 
                                     light.m_attenuattion.y * dist + 
                                     light.m_attenuattion.z * dist * dist);

			//float visibility = evalVisibilityShadowMapping( light_space_pos, surfaceNormal, lightDir, id_light);

            float lightRadius = 0.05;
            float coneAngle = atan(lightRadius / dist);
            int numSamples = 32;
            float visibility = evalVisibilityRTXSoft(fragPosition, surfaceNormal, lightDir, coneAngle, numSamples);

            //float visibility = evalVisibilityRTX(fragPosition, surfaceNormal, lightDir);

            float softVisibility = max(visibility, 0.025);

            Lo = (diffuse + specular) * light.m_radiance.rgb * attenuation * NdotL * softVisibility;
        }

        surfaceColor += Lo;
    }
    
    return surfaceColor;
}





void main() 
{
    float gamma = 2.2f;
    float exposure = 1.0f;
    vec4 material = texture( i_material, f_uvs );
    vec3 mapped = vec3(0.0);
    float AO = texture(i_ssao, f_uvs).r;


    if (material.x == 0.0)
    {
        mapped = vec3( 1.0f ) - exp(-evalDiffuse() * AO * exposure);
    }
    else if (material.x == 1.0)
    {
        mapped = vec3( 1.0f ) - exp(-evalMicrofacets() * AO * exposure);
    }
        

    out_color = vec4( pow( mapped, vec3( 1.0f / gamma ) ), 1.0 );
}