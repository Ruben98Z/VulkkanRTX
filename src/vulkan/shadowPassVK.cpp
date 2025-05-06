#include "common.h"
#include "vulkan/utilsVK.h"
#include "vulkan/rendererVK.h"
#include "vulkan/deviceVK.h"
#include "vulkan/windowVK.h"
#include "runtime.h"
#include "frame.h"
#include "shaderRegistry.h"
#include "entity.h"
#include "vulkan/meshVK.h"
#include "material.h"
#include "vulkan/shadowPassVK.h"

using namespace MiniEngine;

ShadowPassVK::ShadowPassVK(
    const Runtime& i_runtime,
    const ImageBlock& i_shadow_depth_buffer,
    uint32_t i_shadowMapResolution,
    uint32_t i_numShadowMaps) :
    RenderPassVK(i_runtime),
    m_shadow_depth_buffer(i_shadow_depth_buffer),
    m_shadowMapResolution(i_shadowMapResolution),
    m_numShadowMaps(i_numShadowMaps)
{
    // Inicializamos los command buffers a VK_NULL_HANDLE
    for (auto& cmd : m_command_buffer)
    {
        cmd = VK_NULL_HANDLE;
    }
}

ShadowPassVK::~ShadowPassVK() {}

bool ShadowPassVK::initialize()
{
    RendererVK& renderer = *m_runtime.m_renderer;

    
    m_entities_to_draw = {
        { static_cast<uint32_t>(Material::TMaterial::Diffuse), {} },
        { static_cast<uint32_t>(Material::TMaterial::Microfacets), {} }
    };

    {

        VkShaderModule vert_module = m_runtime.m_shader_registry->loadShader("./shaders/vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
        VkShaderModule geom_module = m_runtime.m_shader_registry->loadShader("./shaders/shadows.geom.spv", VK_SHADER_STAGE_GEOMETRY_BIT);


        { // difuse

            VkPipelineShaderStageCreateInfo vert_shader{};
            vert_shader.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            vert_shader.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vert_shader.module = vert_module;
            vert_shader.pName = "main";

            VkPipelineShaderStageCreateInfo geom_shader{};
            geom_shader.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            geom_shader.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
            geom_shader.module = geom_module;
            geom_shader.pName = "main";

            m_pipelines[static_cast<uint32_t>(Material::TMaterial::Diffuse)].m_shader_stages[0] = vert_shader;
            m_pipelines[static_cast<uint32_t>(Material::TMaterial::Diffuse)].m_shader_stages[1] = geom_shader;
            
        }

        { // microfacets
            VkPipelineShaderStageCreateInfo vert_shader{};
            vert_shader.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            vert_shader.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vert_shader.module = vert_module;
            vert_shader.pName = "main";

            VkPipelineShaderStageCreateInfo geom_shader{};
            geom_shader.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            geom_shader.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
            geom_shader.module = geom_module;
            geom_shader.pName = "main";

            m_pipelines[static_cast<uint32_t>(Material::TMaterial::Microfacets)].m_shader_stages[0] = vert_shader;
            m_pipelines[static_cast<uint32_t>(Material::TMaterial::Microfacets)].m_shader_stages[1] = geom_shader;
            
        }
    }
    
    

    
    createRenderPass();
    createPipelines();
    createFbo();

    VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
    commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferAllocateInfo.commandPool = renderer.getDevice()->getCommandPool();
    commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    // Se puede ajustar la cantidad de command buffers según la cantidad de imágenes de la ventana
    commandBufferAllocateInfo.commandBufferCount = 3;

    if (vkAllocateCommandBuffers(renderer.getDevice()->getLogicalDevice(), &commandBufferAllocateInfo, m_command_buffer.data()) != VK_SUCCESS)
    {
        throw MiniEngineException("Error al asignar los command buffers para ShadowPassVK");
    }

    return true;
}

void ShadowPassVK::shutdown()
{
    RendererVK& renderer = *m_runtime.m_renderer;

    vkFreeCommandBuffers(renderer.getDevice()->getLogicalDevice(), renderer.getDevice()->getCommandPool(), m_command_buffer.size(), m_command_buffer.data());

    // Liberar otros recursos, similar a DepthPrePassVK
    vkDestroyDescriptorPool(renderer.getDevice()->getLogicalDevice(), m_descriptor_pool, nullptr);
    for (auto& pipeline : m_pipelines)
    {
        vkDestroyDescriptorSetLayout(renderer.getDevice()->getLogicalDevice(), pipeline.m_descriptor_set_layout[0], nullptr);
        vkDestroyDescriptorSetLayout(renderer.getDevice()->getLogicalDevice(), pipeline.m_descriptor_set_layout[1], nullptr);
        vkDestroyPipeline(renderer.getDevice()->getLogicalDevice(), pipeline.m_pipeline, nullptr);
        vkDestroyPipelineLayout(renderer.getDevice()->getLogicalDevice(), pipeline.m_pipeline_layouts, nullptr);
    }

    // Se destruyen todos los FBO creados
    for (uint32 id = 0; id < static_cast<uint32>(m_fbos.size()); id++)
    {
        vkDestroyFramebuffer(renderer.getDevice()->getLogicalDevice(), m_fbos[id], nullptr);
    }

    vkDestroyRenderPass(renderer.getDevice()->getLogicalDevice(), m_render_pass, nullptr);
}


VkCommandBuffer ShadowPassVK::draw(const Frame& i_frame)
{
    RendererVK& renderer = *m_runtime.m_renderer;
    VkCommandBuffer& current_cmd = m_command_buffer[renderer.getWindow().getCurrentImageId()];

    if (current_cmd != VK_NULL_HANDLE)
    {
        vkResetCommandBuffer(current_cmd, 0);
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(current_cmd, &begin_info) != VK_SUCCESS)
    {
        throw MiniEngineException("Error iniciando el recording del command buffer en ShadowPassVK");
    }

    uint32_t width = m_shadowMapResolution;
    uint32_t height = m_shadowMapResolution;

    VkRenderPassBeginInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass = m_render_pass;
    render_pass_info.framebuffer = m_fbos[renderer.getWindow().getCurrentImageId()];
    render_pass_info.renderArea.offset = { 0, 0 };
    render_pass_info.renderArea.extent = { width, height };

    VkClearValue clear_values[1];
    clear_values[0].depthStencil = { 1.0f, 0 };
    render_pass_info.clearValueCount = 1;
    render_pass_info.pClearValues = clear_values;

    UtilsVK::beginRegion(current_cmd, "Shadow Pass", Vector4f(0.1f, 0.1f, 0.1f, 1.0f));
    vkCmdBeginRenderPass(current_cmd, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    // Bucle de dibujo para las entidades que generan sombras
    for (uint32_t mat_id = static_cast<uint32_t>(Material::TMaterial::Diffuse); mat_id < static_cast<uint32_t>(m_pipelines.size()); mat_id++)
    {
        UtilsVK::beginRegion(current_cmd, "Shadow Pass - Material " + mat_id, Vector4f(0.2f, 0.2f, 0.2f, 1.0f));

        VkDescriptorSet descriptor_sets[] = {
         m_pipelines[mat_id].m_descriptor_sets[renderer.getWindow().getCurrentImageId()].m_per_frame_descriptor,
         m_pipelines[mat_id].m_descriptor_sets[renderer.getWindow().getCurrentImageId()].m_per_object_descriptor,
        };


        vkCmdBindPipeline(current_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines[mat_id].m_pipeline);
        vkCmdBindDescriptorSets(current_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_pipelines[mat_id].m_pipeline_layouts, 0, 2,
            &m_pipelines[mat_id].m_descriptor_sets[renderer.getWindow().getCurrentImageId()].m_per_frame_descriptor,
            0, nullptr);


        for (auto entity : m_entities_to_draw[mat_id])
        {
            // Dibuja la entidad desde la perspectiva de la luz
            entity->draw(current_cmd, i_frame);
        }

        UtilsVK::endRegion(current_cmd);
    }

    vkCmdEndRenderPass(current_cmd);
    UtilsVK::endRegion(current_cmd);

    if (vkEndCommandBuffer(current_cmd) != VK_SUCCESS)
    {
        throw MiniEngineException("Error al finalizar el command buffer en ShadowPassVK");
    }

    return current_cmd;
}

void ShadowPassVK::addEntityToDraw(const EntityPtr i_entity)
{
    m_entities_to_draw[static_cast<uint32_t>(i_entity->getMaterial().getType())].push_back(i_entity);
}

void ShadowPassVK::createFbo()
{
    RendererVK& renderer = *m_runtime.m_renderer;
    // Notar que en este caso la resolución es la del shadow map
    uint32_t width = m_shadowMapResolution;
    uint32_t height = m_shadowMapResolution;

    // Se asume que se crea un framebuffer por imagen de swapchain
    for (size_t i = 0; i < m_fbos.size(); i++)
    {
        // La imagen de profundidad del shadow map es ahora un array (con numShadowMaps layers)
        std::array<VkImageView, 1> attachments;
        attachments[0] = m_shadow_depth_buffer.m_image_view; // Imagen de profundidad para sombras

        VkFramebufferCreateInfo framebuffer_create_info = {};
        framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_create_info.renderPass = m_render_pass;
        framebuffer_create_info.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebuffer_create_info.pAttachments = attachments.data();
        framebuffer_create_info.width = width;
        framebuffer_create_info.height = height;
        // Especificamos el número de layers según el array de imagenes de sombras
        framebuffer_create_info.layers = 10;

        if (vkCreateFramebuffer(renderer.getDevice()->getLogicalDevice(), &framebuffer_create_info, nullptr, &m_fbos[i]) != VK_SUCCESS)
        {
            throw MiniEngineException("Error al crear framebuffer en ShadowPassVK");
        }
    }
}

void ShadowPassVK::createRenderPass()
{
    RendererVK& renderer = *m_runtime.m_renderer;

    std::array<VkAttachmentDescription, 1> attachments = {};

    // Configuración de la attachment de profundidad (similar a DepthPrePass)
    attachments[0].format = m_shadow_depth_buffer.m_format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_reference = {};
    depth_reference.attachment = 0;
    depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass_description = {};
    subpass_description.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    // No se usan color attachments
    subpass_description.colorAttachmentCount = 0;
    subpass_description.pColorAttachments = nullptr;
    subpass_description.pDepthStencilAttachment = &depth_reference;
    subpass_description.inputAttachmentCount = 0;
    subpass_description.pInputAttachments = nullptr;
    subpass_description.preserveAttachmentCount = 0;
    subpass_description.pPreserveAttachments = nullptr;
    subpass_description.pResolveAttachments = nullptr;

    // Dependencias para las transiciones de layout (se pueden ajustar según el uso)
    std::array<VkSubpassDependency, 2> dependencies;
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo render_pass_info = {};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = static_cast<uint32_t>(attachments.size());
    render_pass_info.pAttachments = attachments.data();
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass_description;
    render_pass_info.dependencyCount = static_cast<uint32_t>(dependencies.size());
    render_pass_info.pDependencies = dependencies.data();

    if (vkCreateRenderPass(renderer.getDevice()->getLogicalDevice(), &render_pass_info, nullptr, &m_render_pass) != VK_SUCCESS)
    {
        throw MiniEngineException("Error al crear la render pass en ShadowPassVK");
    }
}

void ShadowPassVK::createPipelines()
{
    RendererVK& renderer = *m_runtime.m_renderer;

    VkVertexInputBindingDescription binding_vertex_descrition{};
    binding_vertex_descrition.binding = 0;
    binding_vertex_descrition.stride = sizeof(Vertex);
    binding_vertex_descrition.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attribute_descriptions{};

    attribute_descriptions[0].binding = 0;
    attribute_descriptions[0].location = 0;
    attribute_descriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attribute_descriptions[0].offset = offsetof(Vertex, m_position);

    attribute_descriptions[1].binding = 0;
    attribute_descriptions[1].location = 1;
    attribute_descriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attribute_descriptions[1].offset = offsetof(Vertex, m_normal);

    attribute_descriptions[2].binding = 0;
    attribute_descriptions[2].location = 2;
    attribute_descriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
    attribute_descriptions[2].offset = offsetof(Vertex, m_uv);

    VkPipelineVertexInputStateCreateInfo vertex_input_info{};
    vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_info.vertexBindingDescriptionCount = 1;
    vertex_input_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribute_descriptions.size());
    vertex_input_info.pVertexBindingDescriptions = &binding_vertex_descrition;
    vertex_input_info.pVertexAttributeDescriptions = attribute_descriptions.data();
    vertex_input_info.flags = 0;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;
    input_assembly.flags = 0;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;
    depth_stencil.flags = 0;

    VkPipelineRasterizationStateCreateInfo raster_info{};
    raster_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster_info.pNext = VK_NULL_HANDLE;
    raster_info.depthClampEnable = VK_FALSE;
    raster_info.rasterizerDiscardEnable = VK_FALSE;
    raster_info.polygonMode = VK_POLYGON_MODE_FILL;
    raster_info.cullMode = VK_CULL_MODE_NONE;
    raster_info.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster_info.depthBiasEnable = VK_TRUE;  // el depth bias 
    raster_info.depthBiasConstantFactor = 1.25f;
    raster_info.depthBiasClamp = 0.0f;
    raster_info.depthBiasSlopeFactor = 1.75f;
    raster_info.lineWidth = 1.f;

    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable = VK_FALSE;

    std::array<VkPipelineColorBlendAttachmentState, 4> blend_state =
    {
        color_blend_attachment,
        color_blend_attachment,
        color_blend_attachment,
        color_blend_attachment
    };

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.logicOpEnable = VK_FALSE;
    color_blending.logicOp = VK_LOGIC_OP_COPY;
    color_blending.attachmentCount = blend_state.size();
    color_blending.pAttachments = blend_state.data();
    color_blending.blendConstants[0] = 0.0f;
    color_blending.blendConstants[1] = 0.0f;
    color_blending.blendConstants[2] = 0.0f;
    color_blending.blendConstants[3] = 0.0f;
    color_blending.flags = 0;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisampling.flags = 0;

    VkExtent2D extend{ m_shadowMapResolution, m_shadowMapResolution };

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = m_shadowMapResolution;
    viewport.height = m_shadowMapResolution;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = extend;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    viewport_state.flags = 0;

    //create unfiorms 
    createDescriptorLayout();

    for (auto& pipeline : m_pipelines)
    {

        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = pipeline.m_descriptor_set_layout.size();
        pipeline_layout_info.pSetLayouts = pipeline.m_descriptor_set_layout.data();
        pipeline_layout_info.pPushConstantRanges = VK_NULL_HANDLE;
        pipeline_layout_info.pushConstantRangeCount = 0;
        pipeline_layout_info.flags = 0;

        std::vector<VkGraphicsPipelineCreateInfo> graphic_pipelines;


        if (vkCreatePipelineLayout(renderer.getDevice()->getLogicalDevice(), &pipeline_layout_info, nullptr, &pipeline.m_pipeline_layouts) != VK_SUCCESS)
        {
            throw MiniEngineException("failed to create pipeline layout!");
        }

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.layout = pipeline.m_pipeline_layouts;
        pipeline_info.renderPass = m_render_pass;
        pipeline_info.basePipelineIndex = -1;
        pipeline_info.basePipelineHandle = VK_NULL_HANDLE;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pRasterizationState = &raster_info;
        pipeline_info.pColorBlendState = &color_blending;
        pipeline_info.pMultisampleState = &multisampling;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pDepthStencilState = &depth_stencil;
        pipeline_info.pDynamicState = VK_NULL_HANDLE;
        pipeline_info.stageCount = pipeline.m_shader_stages.size();
        pipeline_info.pStages = pipeline.m_shader_stages.data();
        pipeline_info.flags = 0;
        pipeline_info.pVertexInputState = &vertex_input_info;
        pipeline_info.subpass = 0;

        graphic_pipelines.push_back(pipeline_info);


        if (vkCreateGraphicsPipelines(renderer.getDevice()->getLogicalDevice(), VK_NULL_HANDLE, 1, graphic_pipelines.data(), nullptr, &pipeline.m_pipeline))
        {
            throw MiniEngineException("Error creating the pipeline");
        }
    }
    createDescriptors();
}



void ShadowPassVK::createDescriptorLayout()
{
    // Binding para el uniform per-frame:
    VkDescriptorSetLayoutBinding per_frame_binding = {};
    per_frame_binding.binding = 0;
    per_frame_binding.descriptorCount = 1;
    per_frame_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    per_frame_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT;

    VkDescriptorSetLayoutCreateInfo set_per_frame_info = {};
    set_per_frame_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_per_frame_info.bindingCount = 1;
    set_per_frame_info.pBindings = &per_frame_binding;

    // Binding para el uniform per-object:
    VkDescriptorSetLayoutBinding per_object_binding = {};
    per_object_binding.binding = 0;
    per_object_binding.descriptorCount = 1;
    per_object_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    per_object_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT;

    VkDescriptorSetLayoutCreateInfo set_per_object_info = {};
    set_per_object_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_per_object_info.bindingCount = 1;
    set_per_object_info.pBindings = &per_object_binding;

    for (auto& pipeline : m_pipelines)
    {
        if (vkCreateDescriptorSetLayout(m_runtime.m_renderer->getDevice()->getLogicalDevice(), &set_per_frame_info, nullptr, &pipeline.m_descriptor_set_layout[0]) != VK_SUCCESS)
        {
            throw MiniEngineException("Error creando descriptor set layout per frame en ShadowPassVK");
        }
        if (vkCreateDescriptorSetLayout(m_runtime.m_renderer->getDevice()->getLogicalDevice(), &set_per_object_info, nullptr, &pipeline.m_descriptor_set_layout[1]) != VK_SUCCESS)
        {
            throw MiniEngineException("Error creando descriptor set layout per object en ShadowPassVK");
        }
    }
}


void ShadowPassVK::createDescriptors()
{
    // Creación del descriptor pool y la asignación de descriptor sets, muy similar a DepthPrePassVK
    std::vector<VkDescriptorPoolSize> sizes = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 30 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 30 }
    };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 30;
    pool_info.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pool_info.pPoolSizes = sizes.data();

    if (vkCreateDescriptorPool(m_runtime.m_renderer->getDevice()->getLogicalDevice(), &pool_info, nullptr, &m_descriptor_pool) != VK_SUCCESS)
    {
        throw MiniEngineException("Error creando descriptor pool en ShadowPassVK");
    }

    for (auto& pipeline : m_pipelines)
    {
        for (uint32_t id = 0; id < m_runtime.m_renderer->getWindow().getImageCount(); id++)
        {
            //globals per frame
            //allocate one descriptor set for each frame
            VkDescriptorSetAllocateInfo alloc_per_frame_info = {};
            alloc_per_frame_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_per_frame_info.descriptorPool = m_descriptor_pool;
            alloc_per_frame_info.descriptorSetCount = 1;
            alloc_per_frame_info.pSetLayouts = &pipeline.m_descriptor_set_layout[0];
            vkAllocateDescriptorSets(m_runtime.m_renderer->getDevice()->getLogicalDevice(), &alloc_per_frame_info, &pipeline.m_descriptor_sets[id].m_per_frame_descriptor);

            //objects
            //allocate one descriptor set for each frame
            VkDescriptorSetAllocateInfo alloc_per_object_info = {};
            alloc_per_object_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_per_object_info.descriptorPool = m_descriptor_pool;
            alloc_per_object_info.descriptorSetCount = 1;
            alloc_per_object_info.pSetLayouts = &pipeline.m_descriptor_set_layout[1];
            vkAllocateDescriptorSets(m_runtime.m_renderer->getDevice()->getLogicalDevice(), &alloc_per_object_info, &pipeline.m_descriptor_sets[id].m_per_object_descriptor);

            //information about the buffer we want to point at in the descriptor
            VkDescriptorBufferInfo binfo[2];
            binfo[0].buffer = m_runtime.getPerFrameBuffer()[id];
            binfo[0].offset = 0;
            binfo[0].range = sizeof(PerFrameData);

            binfo[1].buffer = m_runtime.getPerObjectBuffer()[id];
            binfo[1].offset = 0;
            binfo[1].range = sizeof(PerObjectData) * kMAX_NUMBER_OF_OBJECTS;

            VkWriteDescriptorSet set_write[2] = {};
            set_write[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            set_write[0].dstBinding = 0;
            set_write[0].dstSet = pipeline.m_descriptor_sets[id].m_per_frame_descriptor;
            set_write[0].descriptorCount = 1;
            set_write[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            set_write[0].pBufferInfo = &binfo[0];

            set_write[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            set_write[1].dstBinding = 0;
            set_write[1].dstSet = pipeline.m_descriptor_sets[id].m_per_object_descriptor;
            set_write[1].descriptorCount = 1;
            set_write[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            set_write[1].pBufferInfo = &binfo[1];

            vkUpdateDescriptorSets(m_runtime.m_renderer->getDevice()->getLogicalDevice(),
                2, set_write, 0, nullptr);
        }
    }
}
