#pragma once

#include "vulkan/renderPassVK.h"

namespace MiniEngine
{
    struct Runtime;
    class Entity;
    typedef std::shared_ptr<Entity> EntityPtr;

    class ShadowPassVK final : public RenderPassVK
    {
    public:
        ShadowPassVK(
            const Runtime& i_runtime,
            const ImageBlock& i_shadow_depth_buffer,
            uint32_t i_shadowMapResolution,
            uint32_t i_numShadowMaps);
        virtual ~ShadowPassVK();

        bool initialize() override;
        void shutdown() override;
        VkCommandBuffer draw(const Frame& i_frame) override;

        void addEntityToDraw(const EntityPtr i_entity) override;

    private:
        ShadowPassVK(const ShadowPassVK&) = delete;
        ShadowPassVK& operator=(const ShadowPassVK&) = delete;

        void createFbo();
        void createRenderPass();
        void createPipelines();
        void createDescriptorLayout();
        void createDescriptors();

        struct DescriptorsSets
        {
            VkDescriptorSet m_per_frame_descriptor;
            VkDescriptorSet m_per_object_descriptor;
        };

        struct MaterialPipeline
        {
            // Se preparan pipelines dependiendo del material y del render que se necesite para sombras
            VkPipeline                                                         m_pipeline;
            VkPipelineLayout                                                   m_pipeline_layouts;
            std::array<VkDescriptorSetLayout, 2>                               m_descriptor_set_layout; // Dos sets: per frame y per object.
            std::array<DescriptorsSets, 3>                                     m_descriptor_sets;
            std::array<VkPipelineShaderStageCreateInfo, 2>                     m_shader_stages;
        };

        std::array<MaterialPipeline, 2> m_pipelines; // Uno por cada tipo de material 

        VkRenderPass                   m_render_pass;
        std::array<VkCommandBuffer, 3> m_command_buffer;
        std::array<VkFramebuffer, 3>   m_fbos;
        VkDescriptorPool               m_descriptor_pool;

        std::unordered_map<uint32_t, std::vector<EntityPtr>> m_entities_to_draw;

        ImageBlock m_shadow_depth_buffer;

        
        uint32_t m_shadowMapResolution;
        uint32_t m_numShadowMaps;
    };
}
