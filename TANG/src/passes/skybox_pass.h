#ifndef SKYBOX_PASS_H
#define SKYBOX_PASS_H

#include <array>

#include "../data_buffer/uniform_buffer.h"
#include "../descriptors/descriptor_set.h"
#include "../pipelines/skybox_pipeline.h"
#include "base_pass.h"
#include "../config.h"

namespace TANG
{
	class SkyboxPass : public BasePass
	{
	public:

		SkyboxPass();
		~SkyboxPass();
		SkyboxPass(SkyboxPass&& other) noexcept;

		SkyboxPass(const SkyboxPass& other) = delete;
		SkyboxPass& operator=(const SkyboxPass& other) = delete;

		void SetData(const DescriptorPool* descriptorPool, const HDRRenderPass* hdrRenderPass, VkExtent2D swapChainExtent);

		void UpdateSkyboxCubemap(const TextureResource* skyboxCubemap);
		void UpdateViewProjUniformBuffers(uint32_t frameIndex, const glm::mat4& view, const glm::mat4 proj);

		void UpdateDescriptorSets(uint32_t frameIndex);

		void Create() override;
		void Destroy() override;

		void Draw(uint32_t currentFrame, const DrawData& data);

	private:

		void CreatePipelines() override;
		void CreateSetLayoutCaches() override;
		void CreateDescriptorSets() override;
		void CreateUniformBuffers() override;

		void ResetBorrowedData() override;

		SkyboxPipeline skyboxPipeline;
		SetLayoutCache skyboxSetLayoutCache;
		std::array<UniformBuffer, CONFIG::MaxFramesInFlight> viewUBO;
		std::array<UniformBuffer, CONFIG::MaxFramesInFlight> projUBO;
		std::array<std::array<DescriptorSet, 2>, CONFIG::MaxFramesInFlight> skyboxDescriptorSets;

		struct  
		{
			const DescriptorPool* descriptorPool;
			const HDRRenderPass* hdrRenderPass;
			VkExtent2D swapChainExtent;
		} borrowedData;
	};
}

#endif