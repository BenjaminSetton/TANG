
#include "renderer.h"

// DISABLE WARNINGS FROM GLM
#pragma warning(push)
#pragma warning(disable : 4201 4244)

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_ALIGNED_GENTYPES
#include <glm.hpp>
#include <gtc/matrix_transform.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <gtx/hash.hpp>
#include <gtx/euler_angles.hpp>

#pragma warning(pop) 

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>

// Unfortunately the renderer has to know about GLFW in order to create the surface, since the vulkan call itself
// takes in a GLFWwindow pointer >:(. This also means we have to pass it into the renderer's Initialize() call,
// since the surface has to be initialized for other Vulkan objects to be properly initialized as well...
#include <glfw3.h> // GLFWwindow, glfwCreateWindowSurface() and glfwGetRequiredInstanceExtensions()

#include <glm.hpp>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include "asset_loader.h"
#include "cmd_buffer/disposable_command.h"
#include "command_pool_registry.h"
#include "data_buffer/vertex_buffer.h"
#include "data_buffer/index_buffer.h"
#include "default_material.h"
#include "descriptors/write_descriptor_set.h"
#include "queue_family_indices.h"
#include "utils/file_utils.h"

static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
static constexpr uint32_t MAX_ASSET_COUNT = 100;

static std::vector<const char*> validationLayers = {
	"VK_LAYER_KHRONOS_validation"
};

static std::vector<const char*> deviceExtensions = {
	VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

#ifdef NDEBUG
static const bool enableValidationLayers = false;
#else
static const bool enableValidationLayers = true;
#endif

static const std::string compiledShaderOutputPath = "../out/shaders/pbr";

static VkVertexInputBindingDescription GetVertexBindingDescription()
{
	VkVertexInputBindingDescription bindingDesc{};
	bindingDesc.binding = 0;
	bindingDesc.stride = sizeof(TANG::VertexType);
	bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	return bindingDesc;
}

// Ensure that whenever we update the Vertex layout, we fail to compile unless
// the attribute descriptions below are updated. Note in this case we won't
// assert if the byte usage remains the same but we switch to a different format
// (like switching the order of two attributes)
TNG_ASSERT_COMPILE(sizeof(TANG::VertexType) == 44);

static std::array<VkVertexInputAttributeDescription, 3> GetVertexAttributeDescriptions()
{
	std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};

	// POSITION
	attributeDescriptions[0].binding = 0;
	attributeDescriptions[0].location = 0;
	attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT; // vec3 (12 bytes)
	attributeDescriptions[0].offset = offsetof(TANG::VertexType, pos);

	// NORMAL
	attributeDescriptions[1].binding = 0;
	attributeDescriptions[1].location = 1;
	attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT; // vec3 (12 bytes)
	attributeDescriptions[1].offset = offsetof(TANG::VertexType, normal);

	// TANGENT
	attributeDescriptions[1].binding = 0;
	attributeDescriptions[1].location = 2;
	attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT; // vec3 (12 bytes)
	attributeDescriptions[1].offset = offsetof(TANG::VertexType, tangent);

	// UV
	attributeDescriptions[2].binding = 0;
	attributeDescriptions[2].location = 3;
	attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT; // vec2 (8 bytes)
	attributeDescriptions[2].offset = offsetof(TANG::VertexType, uv);

	return attributeDescriptions;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData
)
{
	UNUSED(pUserData);
	UNUSED(messageType);
	UNUSED(messageSeverity);

	std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;

	return VK_FALSE;
}


VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
	const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger)
{
	auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
	if (func != nullptr) {
		return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
	}
	else {
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
}

void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator)
{
	auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
	if (func != nullptr)
	{
		func(instance, debugMessenger, pAllocator);
	}
}


namespace TANG
{
	struct SwapChainSupportDetails
	{
		VkSurfaceCapabilitiesKHR capabilities;
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR> presentModes;
	};

	// This UBO is updated every frame for every different asset, to properly reflect their location. Matches
	// up with the Transform struct inside asset_types.h
	struct TransformUBO
	{
		TransformUBO() : transform(glm::identity<glm::mat4>())
		{
		}

		TransformUBO(glm::mat4 trans) : transform(trans)
		{
		}

		glm::mat4 transform;
	};

	struct ViewUBO
	{
		glm::mat4 view;
	};

	struct ProjUBO
	{
		glm::mat4 proj;
	};

	// The minimum uniform buffer alignment of the chosen physical device is 64 bytes...an entire matrix 4
	// 
	struct CameraDataUBO
	{
		glm::vec4 position;
		float exposure;
		char padding[44];
	};
	TNG_ASSERT_COMPILE(sizeof(CameraDataUBO) == 64);

	Renderer::Renderer() : 
		vkInstance(VK_NULL_HANDLE), debugMessenger(VK_NULL_HANDLE), physicalDevice(VK_NULL_HANDLE), logicalDevice(VK_NULL_HANDLE), surface(VK_NULL_HANDLE),
		queues(), swapChain(VK_NULL_HANDLE), swapChainImageFormat(VK_FORMAT_UNDEFINED), swapChainExtent({ 0, 0 }), frameDependentData(), swapChainImageDependentData(),
		setLayoutCache(), layoutSummaries(), renderPass(VK_NULL_HANDLE), pipelineLayout(VK_NULL_HANDLE), graphicsPipeline(VK_NULL_HANDLE), currentFrame(0), resourcesMap(),
		assetResources(), descriptorPool(), randomTexture(), depthBuffer(), colorAttachment(), msaaSamples(VK_SAMPLE_COUNT_1_BIT), framebufferWidth(0), framebufferHeight(0)
	{
	}

	void Renderer::Update(float deltaTime)
	{
		UNUSED(deltaTime);

		if (swapChainExtent.width != framebufferWidth || swapChainExtent.height != framebufferHeight)
		{
			RecreateSwapChain();
		}
	}

	void Renderer::Draw()
	{
		DrawFrame();

		// Clear the asset draw states after drawing the current frame
		// TODO - This is pretty slow to do per-frame, so I need to find a better way to
		//        clear the asset draw states. Maybe a sorted pool would work better but
		//        I want to avoid premature optimization so this'll do for now
		for (auto& resources : assetResources)
		{
			resources.shouldDraw = false;
		}

		currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
	}

	// Loads an asset which implies grabbing the vertices and indices from the asset container
	// and creating vertex/index buffers to contain them. It also includes creating all other
	// API objects necessary for rendering. Receives a pointer to a loaded asset. This function
	// assumes the caller handled a null asset correctly
	AssetResources* Renderer::CreateAssetResources(AssetDisk* asset)
	{
		assetResources.emplace_back(AssetResources());
		resourcesMap.insert({ asset->uuid, static_cast<uint32_t>(assetResources.size() - 1) });

		AssetResources& resources = assetResources.back();

		uint32_t meshCount = static_cast<uint32_t>(asset->meshes.size());
		TNG_ASSERT_MSG(meshCount == 1, "Multiple meshes per asset is not currently supported!");

		// Resize the vertex buffer and offset vector to the number of meshes
		resources.vertexBuffers.resize(meshCount);
		resources.offsets.resize(meshCount);

		uint64_t totalIndexCount = 0;
		uint32_t vBufferOffset = 0;

		//////////////////////////////
		//
		//	MESH
		//
		//////////////////////////////
		for (uint32_t i = 0; i < meshCount; i++)
		{
			Mesh& currMesh = asset->meshes[i];

			// Create the vertex buffer
			uint64_t numBytes = currMesh.vertices.size() * sizeof(VertexType);

			VertexBuffer& vb = resources.vertexBuffers[i];
			vb.Create(physicalDevice, logicalDevice, numBytes);

			{
				DisposableCommand command(logicalDevice, QueueType::TRANSFER);
				vb.CopyIntoBuffer(logicalDevice, command.GetBuffer(), currMesh.vertices.data(), numBytes);
			}

			// Create the index buffer
			numBytes = currMesh.indices.size() * sizeof(IndexType);

			IndexBuffer& ib = resources.indexBuffer;
			ib.Create(physicalDevice, logicalDevice, numBytes);

			{
				DisposableCommand command(logicalDevice, QueueType::TRANSFER);
				ib.CopyIntoBuffer(logicalDevice, command.GetBuffer(), currMesh.indices.data(), numBytes);
			}

			// Destroy the staging buffers
			vb.DestroyIntermediateBuffers(logicalDevice);
			ib.DestroyIntermediateBuffers(logicalDevice);

			// Accumulate the index count of this mesh;
			totalIndexCount += currMesh.indices.size();

			// Set the current offset and then increment
			resources.offsets[i] = vBufferOffset++;
		}

		//////////////////////////////
		//
		//	MATERIAL
		//
		//////////////////////////////
		uint32_t numMaterials = static_cast<uint32_t>(asset->materials.size());
		TNG_ASSERT_MSG(numMaterials <= 1, "Multiple materials per asset are not currently supported!");

		if (numMaterials == 0)
		{
			// We need at least _one_ material, even if we didn't deserialize any material information
			// In this case we use a default material (look at default_material.h)
			asset->materials.resize(1);
		}
		Material& material = asset->materials[0];

		// Resize to the number of possible texture types
		resources.material.resize(static_cast<uint32_t>(Material::TEXTURE_TYPE::_COUNT));

		// Pre-emptively fill out the texture create info, so we can just pass it to all CreateFromFile() calls
		SamplerCreateInfo samplerInfo{};
		samplerInfo.minificationFilter = VK_FILTER_LINEAR;
		samplerInfo.magnificationFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeUVW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.maxAnisotropy = 1.0f; // Is this an appropriate value??

		ImageViewCreateInfo viewCreateInfo{};
		viewCreateInfo.aspect = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

		for (uint32_t i = 0; i < static_cast<uint32_t>(Material::TEXTURE_TYPE::_COUNT); i++)
		{
			Material::TEXTURE_TYPE texType = static_cast<Material::TEXTURE_TYPE>(i);
			if (material.HasTextureOfType(texType))
			{
				Texture* matTexture = material.GetTextureOfType(texType);
				TNG_ASSERT_MSG(matTexture != nullptr, "Why is this texture nullptr when we specifically checked against it?");

				TextureResource& texResource = resources.material[i];
				texResource.CreateFromFile(physicalDevice, logicalDevice, matTexture->fileName, &viewCreateInfo, &samplerInfo);
			}
			else
			{
				// Create a fallback for use in the shader
				BaseImageCreateInfo baseImageInfo{};
				baseImageInfo.width = 1;
				baseImageInfo.height = 1;
				baseImageInfo.mipLevels = 1;
				baseImageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
				baseImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
				baseImageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

				uint32_t data = DEFAULT_MATERIAL.at(texType);

				TextureResource& texResource = resources.material[i];
				texResource.Create(physicalDevice, logicalDevice, &baseImageInfo, &viewCreateInfo, &samplerInfo);
				texResource.CopyDataIntoImage(physicalDevice, logicalDevice, static_cast<void*>(&data), sizeof(data));
				texResource.TransitionLayout(logicalDevice, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			}
		}

		// Insert the asset's uuid into the assetDrawState map. We do not render it
		// upon insertion by default
		resources.shouldDraw = false;
		resources.transform = Transform();
		resources.indexCount = totalIndexCount;
		resources.uuid = asset->uuid;

		// Create uniform buffers for this asset
		CreateAssetUniformBuffers(resources.uuid);

		CreateDescriptorSets(resources.uuid);

		// Initialize the view + projection matrix UBOs to some values, so when new assets are created they get sensible defaults
		// for their descriptor sets. 
		// Note that we're operating under the assumption that assets will only be created before
		// we hit the update loop, simply because we're updating all frames in flight here. If this changes in the future, another
		// solution must be implemented
		glm::vec3 pos = { 0.0f, 5.0f, 15.0f };
		glm::vec3 eye = { 0.0f, 0.0f, 1.0f };
		glm::mat4 viewMat = glm::inverse(glm::lookAt(pos, pos + eye, { 0.0f, 1.0f, 0.0f }));  // TODO - Remove this hard-coded stuff
		for (uint32_t i = 0; i < GetFDDSize(); i++)
		{

			UpdateCameraDataUniformBuffers(resources.uuid, i, pos, viewMat);
			UpdateProjectionUniformBuffer(resources.uuid, i);

			InitializeDescriptorSets(resources.uuid, i);
		}


		return &resources;
	}

	void Renderer::CreateAssetCommandBuffer(AssetResources* resources)
	{
		UUID assetID = resources->uuid;

		// For every swap-chain image, insert object into map and then grab a reference to it
		for (uint32_t i = 0; i < GetSWIDDSize(); i++)
		{
			auto& secondaryCmdBufferMap = GetSWIDDAtIndex(i)->secondaryCommandBuffer;
			// Ensure that there's not already an entry in the secondaryCommandBuffers map. We bail in case of a collision
			if (secondaryCmdBufferMap.find(assetID) != secondaryCmdBufferMap.end())
			{
				LogError("Attempted to create a secondary command buffer for an asset, but a secondary command buffer was already found for asset uuid %ull", assetID);
				return;
			}

			secondaryCmdBufferMap.emplace(assetID, SecondaryCommandBuffer());
			SecondaryCommandBuffer& commandBuffer = secondaryCmdBufferMap[assetID];
			commandBuffer.Create(logicalDevice, CommandPoolRegistry::GetInstance().GetCommandPool(QueueType::GRAPHICS));
		}
	}

	void Renderer::DestroyAssetBuffersHelper(AssetResources& resources)
	{
		// Destroy all vertex buffers
		for (auto& vb : resources.vertexBuffers)
		{
			vb.Destroy(logicalDevice);
		}

		// Destroy the index buffer
		resources.indexBuffer.Destroy(logicalDevice);

		// Destroy textures
		for (auto& tex : resources.material)
		{
			tex.Destroy(logicalDevice);
		}
	}

	PrimaryCommandBuffer* Renderer::GetCurrentPrimaryBuffer()
	{
		return &GetCurrentFDD()->primaryCommandBuffer;
	}

	SecondaryCommandBuffer* Renderer::GetSecondaryCommandBufferAtIndex(uint32_t frameBufferIndex, UUID uuid)
	{
		return &GetSWIDDAtIndex(frameBufferIndex)->secondaryCommandBuffer.at(uuid);
	}

	VkFramebuffer Renderer::GetFramebufferAtIndex(uint32_t frameBufferIndex)
	{
		return GetSWIDDAtIndex(frameBufferIndex)->swapChainFramebuffer;
	}

	void Renderer::DestroyAssetResources(UUID uuid)
	{
		TNG_ASSERT_MSG(resourcesMap.find(uuid) != resourcesMap.end(), "Failed to find asset resources!");

		// Destroy the resources
		DestroyAssetBuffersHelper(assetResources[resourcesMap[uuid]]);

		// Remove resources from the vector
		uint32_t resourceIndex = static_cast<uint32_t>((&assetResources[resourcesMap[uuid]] - &assetResources[0]) / sizeof(assetResources));
		assetResources.erase(assetResources.begin() + resourceIndex);

		// Destroy reference to resources
		resourcesMap.erase(uuid);
	}

	void Renderer::DestroyAllAssetResources()
	{
		uint32_t numAssetResources = static_cast<uint32_t>(assetResources.size());

		for (uint32_t i = 0; i < numAssetResources; i++)
		{
			DestroyAssetBuffersHelper(assetResources[i]);
		}

		assetResources.clear();
		resourcesMap.clear();
	}

	void Renderer::CreateSurface(GLFWwindow* windowHandle)
	{
		if (glfwCreateWindowSurface(vkInstance, windowHandle, nullptr, &surface) != VK_SUCCESS)
		{
			TNG_ASSERT_MSG(false, "Failed to create window surface!");
		}
	}

	void Renderer::SetNextFramebufferSize(uint32_t newWidth, uint32_t newHeight)
	{
		framebufferWidth = newWidth;
		framebufferHeight = newHeight;
	}

	void Renderer::UpdateCameraData(const glm::vec3& position, const glm::mat4& viewMatrix)
	{
		FrameDependentData* currentFDD = GetCurrentFDD();
		auto& assetDescriptorMap = currentFDD->assetDescriptorDataMap;

		// Update the view matrix and camera position UBOs for all assets, as well as the descriptor sets
		for (auto& assetData : assetDescriptorMap)
		{
			UpdateCameraDataUniformBuffers(assetData.first, currentFrame, position, viewMatrix);
			UpdateCameraDataDescriptorSet(assetData.first, currentFrame);
		}
	}

	void Renderer::RecreateSwapChain()
	{
		vkDeviceWaitIdle(logicalDevice);

		CleanupSwapChain();

		CreateSwapChain();
		CreateColorAttachmentTexture();
		CreateDepthTexture();
		CreateFramebuffers();
		RecreateAllSecondaryCommandBuffers();
	}

	void Renderer::Initialize(GLFWwindow* windowHandle, uint32_t windowWidth, uint32_t windowHeight)
	{
		frameDependentData.resize(MAX_FRAMES_IN_FLIGHT);
		framebufferWidth = windowWidth;
		framebufferHeight = windowHeight;

		// Initialize Vulkan-related objects
		CreateInstance();
		SetupDebugMessenger();
		CreateSurface(windowHandle);
		PickPhysicalDevice();
		CreateLogicalDevice();
		CreateSwapChain();
		CreateDescriptorSetLayouts();
		CreateDescriptorPool();
		CreateRenderPass();
		CreateGraphicsPipeline();
		CreateCommandPools();
		CreateColorAttachmentTexture();
		CreateDepthTexture();
		CreateFramebuffers();
		CreatePrimaryCommandBuffers(QueueType::GRAPHICS);
		CreateSyncObjects();
	}

	void Renderer::Shutdown()
	{
		vkDeviceWaitIdle(logicalDevice);

		DestroyAllAssetResources();

		CleanupSwapChain();

		randomTexture.Destroy(logicalDevice);

		setLayoutCache.DestroyLayouts(logicalDevice);

		for (uint32_t i = 0; i < GetFDDSize(); i++)
		{
			auto frameData = GetFDDAtIndex(i);
			for (auto& iter : frameData->assetDescriptorDataMap)
			{
				iter.second.transformUBO.Destroy(logicalDevice);
				iter.second.projUBO.Destroy(logicalDevice);
				iter.second.viewUBO.Destroy(logicalDevice);
				iter.second.cameraDataUBO.Destroy(logicalDevice);
			}

		}

		descriptorPool.Destroy(logicalDevice);

		for (uint32_t i = 0; i < GetFDDSize(); i++)
		{
			auto frameData = GetFDDAtIndex(i);
			vkDestroySemaphore(logicalDevice, frameData->imageAvailableSemaphore, nullptr);
			vkDestroySemaphore(logicalDevice, frameData->renderFinishedSemaphore, nullptr);
			vkDestroyFence(logicalDevice, frameData->inFlightFence, nullptr);
		}

		CommandPoolRegistry::GetInstance().DestroyPools(logicalDevice);

		vkDestroyPipeline(logicalDevice, graphicsPipeline, nullptr);
		vkDestroyPipelineLayout(logicalDevice, pipelineLayout, nullptr);
		vkDestroyRenderPass(logicalDevice, renderPass, nullptr);

		vkDestroyDevice(logicalDevice, nullptr);
		vkDestroySurfaceKHR(vkInstance, surface, nullptr);

		if (enableValidationLayers)
		{
			DestroyDebugUtilsMessengerEXT(vkInstance, debugMessenger, nullptr);
		}

		vkDestroyInstance(vkInstance, nullptr);
	}

	void Renderer::SetAssetDrawState(UUID uuid)
	{
		if (resourcesMap.find(uuid) == resourcesMap.end())
		{
			// Undefined behavior. Maybe the asset resources were deleted but we somehow forgot to remove it from the assetDrawStates map?
			LogError(false, "Attempted to set asset draw state, but draw state doesn't exist in the map!");
		}

		assetResources[resourcesMap[uuid]].shouldDraw = true;
	}

	void Renderer::SetAssetTransform(UUID uuid, Transform& transform)
	{
		assetResources[resourcesMap[uuid]].transform = transform;
	}

	void Renderer::SetAssetPosition(UUID uuid, glm::vec3& position)
	{
		Transform& transform = assetResources[resourcesMap[uuid]].transform;
		transform.position = position;
	}

	void Renderer::SetAssetRotation(UUID uuid, glm::vec3& rotation)
	{
		Transform& transform = assetResources[resourcesMap[uuid]].transform;
		transform.rotation = rotation;
	}

	void Renderer::SetAssetScale(UUID uuid, glm::vec3& scale)
	{
		Transform& transform = assetResources[resourcesMap[uuid]].transform;
		transform.scale = scale;
	}

	void Renderer::DrawFrame()
	{
		VkResult result = VK_SUCCESS;

		FrameDependentData* currentFDD = GetCurrentFDD();

		vkWaitForFences(logicalDevice, 1, &currentFDD->inFlightFence, VK_TRUE, UINT64_MAX);

		uint32_t imageIndex;
		result = vkAcquireNextImageKHR(logicalDevice, swapChain, UINT64_MAX,
			currentFDD->imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);

		if (result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			RecreateSwapChain();
			return;
		}
		else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		{
			TNG_ASSERT_MSG(false, "Failed to acquire swap chain image!");
		}

		// Only reset the fence if we're submitting work, otherwise we might deadlock
		vkResetFences(logicalDevice, 1, &(currentFDD->inFlightFence));

		///////////////////////////////////////
		// 
		// Record and submit primary command buffer
		//
		///////////////////////////////////////
		RecordPrimaryCommandBuffer(imageIndex);

		VkCommandBuffer commandBuffers[] = { GetCurrentPrimaryBuffer()->GetBuffer() };
		VkSemaphore waitSemaphores[] = { currentFDD->imageAvailableSemaphore };

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = commandBuffers;

		VkSemaphore signalSemaphores[] = { currentFDD->renderFinishedSemaphore };
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = signalSemaphores;

		if (SubmitQueue(QueueType::GRAPHICS, &submitInfo, 1, currentFDD->inFlightFence) != VK_SUCCESS)
		{
			return;
		}

		///////////////////////////////////////
		// 
		// Swap chain present
		//
		///////////////////////////////////////
		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;

		VkSwapchainKHR swapChains[] = { swapChain };
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = swapChains;
		presentInfo.pImageIndices = &imageIndex;
		presentInfo.pResults = nullptr;

		result = vkQueuePresentKHR(queues[QueueType::PRESENT], &presentInfo);

		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		{
			RecreateSwapChain();
		}
		else if (result != VK_SUCCESS)
		{
			LogError("Failed to present swap chain image!");
		}
	}

	void Renderer::CreateInstance()
	{
		// Check that we support all requested validation layers
		if (enableValidationLayers && !CheckValidationLayerSupport())
		{
			LogError("Validation layers were requested, but one or more is not supported!");
		}

		VkApplicationInfo appInfo{};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "TANG";
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "No Engine";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.apiVersion = VK_API_VERSION_1_0;

		VkInstanceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		createInfo.pApplicationInfo = &appInfo;

		VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
		if (enableValidationLayers)
		{
			createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
			createInfo.ppEnabledLayerNames = validationLayers.data();

			PopulateDebugMessengerCreateInfo(debugCreateInfo);
			createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debugCreateInfo;
		}
		else
		{
			createInfo.enabledLayerCount = 0;
			createInfo.pNext = nullptr;
		}

		auto extensions = GetRequiredExtensions();

		createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
		createInfo.ppEnabledExtensionNames = extensions.data();
		createInfo.enabledLayerCount = 0;

		if (vkCreateInstance(&createInfo, nullptr, &vkInstance) != VK_SUCCESS)
		{
			TNG_ASSERT_MSG(false, "Failed to create Vulkan instance!");
		}
	}

	void Renderer::PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo)
	{
		createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		createInfo.messageSeverity = /*VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | */VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
		createInfo.pfnUserCallback = debugCallback;
		createInfo.pUserData = nullptr; // Optional
	}

	void Renderer::SetupDebugMessenger()
	{
		if (!enableValidationLayers) return;

		VkDebugUtilsMessengerCreateInfoEXT createInfo{};
		PopulateDebugMessengerCreateInfo(createInfo);

		if (CreateDebugUtilsMessengerEXT(vkInstance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to setup debug messenger!");
		}
	}

	std::vector<const char*> Renderer::GetRequiredExtensions()
	{
		uint32_t glfwExtensionCount = 0;
		const char** glfwExtensions;
		glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

		std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

		if (enableValidationLayers)
		{
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}

		return extensions;
	}

	bool Renderer::CheckValidationLayerSupport()
	{
		uint32_t layerCount;
		vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

		std::vector<VkLayerProperties> availableLayers(layerCount);
		vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

		for (const char* layerName : validationLayers)
		{
			bool layerFound = false;

			for (const auto& layerProperties : availableLayers)
			{
				if (strcmp(layerName, layerProperties.layerName) == 0)
				{
					layerFound = true;
					break;
				}
			}

			if (!layerFound)
			{
				return false;
			}
		}

		return true;
	}

	bool Renderer::CheckDeviceExtensionSupport(VkPhysicalDevice device)
	{
		uint32_t extensionCount;
		vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

		std::vector<VkExtensionProperties> availableExtensions(extensionCount);
		vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

		std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());
		for (const auto& extension : availableExtensions)
		{
			requiredExtensions.erase(extension.extensionName);
		}

		return requiredExtensions.empty();
	}

	////////////////////////////////////////
	//
	//  PHYSICAL DEVICE
	//
	////////////////////////////////////////
	void Renderer::PickPhysicalDevice()
	{
		uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(vkInstance, &deviceCount, nullptr);

		if (deviceCount == 0)
		{
			TNG_ASSERT_MSG(false, "Failed to find GPU with Vulkan support");
		}

		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(vkInstance, &deviceCount, devices.data());

		for (const auto& device : devices)
		{
			if (IsDeviceSuitable(device))
			{
				physicalDevice = device;
				msaaSamples = GetMaxUsableSampleCount();
				break;
			}
		}

		if (physicalDevice == VK_NULL_HANDLE)
		{
			TNG_ASSERT_MSG(false, "Failed to find suitable device (GPU)!");
		}
	}

	bool Renderer::IsDeviceSuitable(VkPhysicalDevice device)
	{
		QueueFamilyIndices indices = FindQueueFamilies(device, surface);
		bool extensionsSupported = CheckDeviceExtensionSupport(device);
		bool swapChainAdequate = false;
		if (extensionsSupported)
		{
			SwapChainSupportDetails details = QuerySwapChainSupport(device);
			swapChainAdequate = !details.formats.empty() && !details.presentModes.empty();
		}

		VkPhysicalDeviceFeatures supportedFeatures;
		vkGetPhysicalDeviceFeatures(device, &supportedFeatures);

		return indices.IsComplete() && extensionsSupported && swapChainAdequate && supportedFeatures.samplerAnisotropy;

		//
		// THE CODE BELOW IS AN EXAMPLE OF HOW TO SELECT DEDICATED GPUS ONLY, WHILE
		// IGNORING INTEGRATED GPUS. FOR THE SAKE OF THIS EXAMPLE, WE'LL CONSIDER ALL
		// GPUS TO BE SUITABLE.
		//

		//VkPhysicalDeviceProperties properties;
		//vkGetPhysicalDeviceProperties(device, &properties);

		//VkPhysicalDeviceFeatures features;
		//vkGetPhysicalDeviceFeatures(device, &features);

		//// We're only going to say that dedicated GPUs are suitable, let's not deal with
		//// integrated graphics for now
		//return properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
	}

	SwapChainSupportDetails Renderer::QuerySwapChainSupport(VkPhysicalDevice device)
	{
		SwapChainSupportDetails details;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

		uint32_t formatCount;
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

		if (formatCount != 0)
		{
			details.formats.resize(formatCount);
			vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
		}

		uint32_t presentModeCount;
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);

		if (presentModeCount != 0)
		{
			details.presentModes.resize(presentModeCount);
			vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
		}

		return details;
	}

	VkSurfaceFormatKHR Renderer::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats)
	{
		for (const auto& availableFormat : availableFormats)
		{
			if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				return availableFormat;
			}
		}

		// We don't check if available formats is empty!
		return availableFormats[0];
	}

	VkPresentModeKHR Renderer::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes)
	{
		for (const auto& presentMode : availablePresentModes)
		{
			if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
			{
				return presentMode;
			}
		}

		return VK_PRESENT_MODE_FIFO_KHR;
	}

	VkExtent2D Renderer::ChooseSwapChainExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t actualWidth, uint32_t actualHeight)
	{
		if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
		{
			return capabilities.currentExtent;
		}
		else
		{
			VkExtent2D actualExtent =
			{
				static_cast<uint32_t>(actualWidth),
				static_cast<uint32_t>(actualHeight)
			};

			actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width,
				capabilities.maxImageExtent.width);
			actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height,
				capabilities.maxImageExtent.height);

			return actualExtent;

		}
	}

	uint32_t Renderer::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
	{
		VkPhysicalDeviceMemoryProperties memProperties;
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

		for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
		{
			if (typeFilter & (1 << i) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
			{
				return i;
			}
		}

		TNG_ASSERT_MSG(false, "Failed to find suitable memory type!");
		return std::numeric_limits<uint32_t>::max();
	}

	void Renderer::CreateLogicalDevice()
	{
		QueueFamilyIndices indices = FindQueueFamilies(physicalDevice, surface);
		if (!indices.IsComplete())
		{
			LogError("Failed to create logical device because the queue family indices are incomplete!");
		}

		std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
		std::set<uint32_t> uniqueQueueFamilies = {
			indices.GetIndex(QueueType::GRAPHICS),
			indices.GetIndex(QueueType::PRESENT),
			indices.GetIndex(QueueType::TRANSFER)
		};

		// TODO - Determine priority of the different queue types
		float queuePriority = 1.0f;
		for (uint32_t queueFamily : uniqueQueueFamilies)
		{
			VkDeviceQueueCreateInfo queueCreateInfo{};
			queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueCreateInfo.queueFamilyIndex = queueFamily;
			queueCreateInfo.queueCount = 1;
			queueCreateInfo.pQueuePriorities = &queuePriority;
			queueCreateInfos.push_back(queueCreateInfo);
		}

		VkPhysicalDeviceFeatures deviceFeatures{};
		deviceFeatures.samplerAnisotropy = VK_TRUE;

		VkDeviceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		createInfo.pQueueCreateInfos = queueCreateInfos.data();
		createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
		createInfo.pEnabledFeatures = &deviceFeatures;
		createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
		createInfo.ppEnabledExtensionNames = deviceExtensions.data();

		if (enableValidationLayers)
		{
			// We get a warning about using deprecated and ignored 'ppEnabledLayerNames', so I've commented these out.
			// It looks like validation layers work regardless...somehow...
			//createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
			//createInfo.ppEnabledLayerNames = validationLayers.data();
		}
		else
		{
			createInfo.enabledLayerCount = 0;
		}

		if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &logicalDevice) != VK_SUCCESS)
		{
			TNG_ASSERT_MSG(false, "Failed to create the logical device!");
		}

		// Get the queues from the logical device
		vkGetDeviceQueue(logicalDevice, indices.GetIndex(QueueType::GRAPHICS), 0, &queues[QueueType::GRAPHICS]);
		vkGetDeviceQueue(logicalDevice, indices.GetIndex(QueueType::PRESENT), 0, &queues[QueueType::PRESENT]);
		vkGetDeviceQueue(logicalDevice, indices.GetIndex(QueueType::TRANSFER), 0, &queues[QueueType::TRANSFER]);

	}

	void Renderer::CreateSwapChain()
	{
		SwapChainSupportDetails details = QuerySwapChainSupport(physicalDevice);
		VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(details.formats);
		VkPresentModeKHR presentMode = ChooseSwapPresentMode(details.presentModes);
		VkExtent2D extent = ChooseSwapChainExtent(details.capabilities, framebufferWidth, framebufferHeight);

		uint32_t imageCount = details.capabilities.minImageCount + 1;
		if (details.capabilities.maxImageCount > 0 && imageCount > details.capabilities.maxImageCount)
		{
			imageCount = details.capabilities.maxImageCount;
		}

		VkSwapchainCreateInfoKHR createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		createInfo.surface = surface;
		createInfo.minImageCount = imageCount;
		createInfo.imageFormat = surfaceFormat.format;
		createInfo.imageColorSpace = surfaceFormat.colorSpace;
		createInfo.imageExtent = extent;
		createInfo.imageArrayLayers = 1;
		createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

		QueueFamilyIndices indices = FindQueueFamilies(physicalDevice, surface);
		uint32_t queueFamilyIndices[2] = { indices.GetIndex(QueueType::GRAPHICS), indices.GetIndex(QueueType::PRESENT)};

		if (queueFamilyIndices[0] != queueFamilyIndices[1])
		{
			createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
			createInfo.queueFamilyIndexCount = 2;
			createInfo.pQueueFamilyIndices = queueFamilyIndices;
		}
		else
		{
			createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			createInfo.queueFamilyIndexCount = 0; // Optional
			createInfo.pQueueFamilyIndices = nullptr; // Optional
		}

		createInfo.preTransform = details.capabilities.currentTransform;
		createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		createInfo.presentMode = presentMode;
		createInfo.clipped = VK_TRUE;
		createInfo.oldSwapchain = VK_NULL_HANDLE;

		if (vkCreateSwapchainKHR(logicalDevice, &createInfo, nullptr, &swapChain) != VK_SUCCESS)
		{
			TNG_ASSERT_MSG(false, "Failed to create swap chain!");
		}

		// Get the number of images, then we use the count to create the image views below
		vkGetSwapchainImagesKHR(logicalDevice, swapChain, &imageCount, nullptr);

		swapChainImageFormat = surfaceFormat.format;
		swapChainExtent = extent;

		CreateSwapChainImageViews(imageCount);
	}

	// Create image views for all images on the swap chain
	void Renderer::CreateSwapChainImageViews(uint32_t imageCount)
	{
		swapChainImageDependentData.resize(imageCount);
		std::vector<VkImage> swapChainImages(imageCount);

		vkGetSwapchainImagesKHR(logicalDevice, swapChain, &imageCount, swapChainImages.data());

		for (uint32_t i = 0; i < imageCount; i++)
		{
			swapChainImageDependentData[i].swapChainImage.CreateImageViewFromBase(logicalDevice, swapChainImages[i], swapChainImageFormat, 1, VK_IMAGE_ASPECT_COLOR_BIT);
		}
		swapChainImages.clear();
	}

	// This is a helper function for creating the "VkShaderModule" wrappers around
	// the shader code, read from CreateGraphicsPipeline() below
	VkShaderModule Renderer::CreateShaderModule(const char* shaderCode, uint32_t numBytes)
	{
		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = numBytes;
		createInfo.pCode = reinterpret_cast<const uint32_t*>(shaderCode);

		VkShaderModule shaderModule;
		if (vkCreateShaderModule(logicalDevice, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
		{
			TNG_ASSERT_MSG(false, "Failed to create shader module!");
		}

		return shaderModule;
	}

	void Renderer::CreateCommandPools()
	{
		CommandPoolRegistry::GetInstance().CreatePools(physicalDevice, logicalDevice, surface);
	}

	void Renderer::CreateGraphicsPipeline()
	{
		// Read the compiled shaders
		VkShaderModule vertShaderModule = LoadShader("vert.spv");
		VkShaderModule fragShaderModule = LoadShader("frag.spv");

		VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
		vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
		vertShaderStageInfo.module = vertShaderModule;
		vertShaderStageInfo.pName = "main";

		VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
		fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		fragShaderStageInfo.module = fragShaderModule;
		fragShaderStageInfo.pName = "main";

		VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

		// Vertex input
		VkPipelineVertexInputStateCreateInfo vertexInputInfo{};

		auto bindingDescription = GetVertexBindingDescription();
		auto attributeDescriptions = GetVertexAttributeDescriptions();
		vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInputInfo.vertexBindingDescriptionCount = 1;
		vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
		vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
		vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

		// Input assembler
		VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		inputAssembly.primitiveRestartEnable = VK_FALSE;

		// Viewports and scissors
		VkViewport viewport{};
		viewport.x = 0.0f;
		viewport.y = 0.0f;
		viewport.width = (float)swapChainExtent.width;
		viewport.height = (float)swapChainExtent.height;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;

		VkRect2D scissor{};
		scissor.offset = { 0, 0 };
		scissor.extent = swapChainExtent;

		// We're declaring these as dynamic states, meaning we can change
		// them at any point. Usually the pipeline states in Vulkan are static,
		// meaning a pipeline is created and never changed. This allows
		// the GPU to heavily optimize for the pipelines defined. In this
		// case though, we face a negligible penalty for making these dynamic.
		std::vector<VkDynamicState> dynamicStates =
		{
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR
		};
		VkPipelineDynamicStateCreateInfo dynamicState{};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
		dynamicState.pDynamicStates = dynamicStates.data();

		VkPipelineViewportStateCreateInfo viewportState{};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		// Rasterizer
		VkPipelineRasterizationStateCreateInfo rasterizer{};
		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.depthClampEnable = VK_FALSE;
		rasterizer.rasterizerDiscardEnable = VK_FALSE;
		rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
		// For the polygonMode it's possible to use LINE or POINT as well
		// In this case the following line is required:
		rasterizer.lineWidth = 1.0f;
		// Any line thicker than 1.0 requires the "wideLines" GPU feature
		rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
		rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterizer.depthBiasEnable = VK_FALSE;
		rasterizer.depthBiasConstantFactor = 0.0f; // Optional
		rasterizer.depthBiasClamp = 0.0f; // Optional
		rasterizer.depthBiasSlopeFactor = 0.0f; // Optional

		// Multisampling
		VkPipelineMultisampleStateCreateInfo multisampling{};
		multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisampling.sampleShadingEnable = VK_FALSE;
		multisampling.rasterizationSamples = msaaSamples;
		multisampling.minSampleShading = 1.0f; // Optional
		multisampling.pSampleMask = nullptr; // Optional
		multisampling.alphaToCoverageEnable = VK_FALSE; // Optional
		multisampling.alphaToOneEnable = VK_FALSE; // Optional

		// Color blending
		VkPipelineColorBlendAttachmentState colorBlendAttachment{};
		colorBlendAttachment.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT |
			VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT |
			VK_COLOR_COMPONENT_A_BIT;
		colorBlendAttachment.blendEnable = VK_FALSE;
		colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
		colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
		colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD; // Optional
		colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
		colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
		colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD; // Optional

		VkPipelineColorBlendStateCreateInfo colorBlending{};
		colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlending.logicOpEnable = VK_FALSE;
		colorBlending.logicOp = VK_LOGIC_OP_COPY; // Optional
		colorBlending.attachmentCount = 1;
		colorBlending.pAttachments = &colorBlendAttachment;
		colorBlending.blendConstants[0] = 0.0f; // Optional
		colorBlending.blendConstants[1] = 0.0f; // Optional
		colorBlending.blendConstants[2] = 0.0f; // Optional
		colorBlending.blendConstants[3] = 0.0f; // Optional

		// Depth stencil
		VkPipelineDepthStencilStateCreateInfo depthStencil{};
		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencil.depthTestEnable = VK_TRUE;
		depthStencil.depthWriteEnable = VK_TRUE;
		depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
		depthStencil.depthBoundsTestEnable = VK_FALSE;
		depthStencil.minDepthBounds = 0.0f; // Optional
		depthStencil.maxDepthBounds = 1.0f; // Optional
		depthStencil.stencilTestEnable = VK_FALSE;
		depthStencil.front = {}; // Optional
		depthStencil.back = {}; // Optional

		// Pipeline layout
		std::vector<VkDescriptorSetLayout> vkDescSetLayouts;
		LayoutCache& cache = setLayoutCache.GetLayoutCache();
		for (auto& iter : cache)
		{
			vkDescSetLayouts.push_back(iter.second.GetLayout());
		}

		VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = setLayoutCache.GetLayoutCount();
		pipelineLayoutInfo.pSetLayouts = vkDescSetLayouts.data();
		pipelineLayoutInfo.pushConstantRangeCount = 0; // Optional
		pipelineLayoutInfo.pPushConstantRanges = nullptr; // Optional

		if (vkCreatePipelineLayout(logicalDevice, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
		{
			TNG_ASSERT_MSG(false, "Failed to create pipeline layout!");
		}

		VkGraphicsPipelineCreateInfo pipelineInfo{};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineInfo.stageCount = 2;
		pipelineInfo.pStages = shaderStages;
		pipelineInfo.pVertexInputState = &vertexInputInfo;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &rasterizer;
		pipelineInfo.pMultisampleState = &multisampling;
		pipelineInfo.pDepthStencilState = &depthStencil;
		pipelineInfo.pColorBlendState = &colorBlending;
		pipelineInfo.pDynamicState = &dynamicState;
		pipelineInfo.layout = pipelineLayout;
		pipelineInfo.renderPass = renderPass;
		pipelineInfo.subpass = 0;
		pipelineInfo.basePipelineHandle = VK_NULL_HANDLE; // Optional
		pipelineInfo.basePipelineIndex = -1; // Optional

		if (vkCreateGraphicsPipelines(logicalDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS)
		{
			TNG_ASSERT_MSG(false, "Failed to create graphics pipeline!");
		}

		vkDestroyShaderModule(logicalDevice, fragShaderModule, nullptr);
		vkDestroyShaderModule(logicalDevice, vertShaderModule, nullptr);

	}

	void Renderer::CreateRenderPass()
	{
		VkAttachmentDescription colorAttachmentDesc{};
		colorAttachmentDesc.format = swapChainImageFormat;
		colorAttachmentDesc.samples = msaaSamples;
		colorAttachmentDesc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachmentDesc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachmentDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachmentDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachmentDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachmentDesc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference colorAttachmentRef{};
		colorAttachmentRef.attachment = 0;
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentDescription depthAttachmentDesc{};
		depthAttachmentDesc.format = FindDepthFormat();
		depthAttachmentDesc.samples = msaaSamples;
		depthAttachmentDesc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachmentDesc.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachmentDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthAttachmentDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachmentDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		depthAttachmentDesc.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentReference depthAttachmentRef{};
		depthAttachmentRef.attachment = 1;
		depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentDescription colorAttachmentResolve{};
		colorAttachmentResolve.format = swapChainImageFormat;
		colorAttachmentResolve.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachmentResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachmentResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachmentResolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachmentResolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachmentResolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachmentResolve.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference colorAttachmentResolveRef{};
		colorAttachmentResolveRef.attachment = 2;
		colorAttachmentResolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorAttachmentRef;
		subpass.pDepthStencilAttachment = &depthAttachmentRef;
		subpass.pResolveAttachments = &colorAttachmentResolveRef;

		VkSubpassDependency dependency{};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		std::array<VkAttachmentDescription, 3> attachments = { colorAttachmentDesc, depthAttachmentDesc, colorAttachmentResolve };
		VkRenderPassCreateInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		renderPassInfo.pAttachments = attachments.data();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 1;
		renderPassInfo.pDependencies = &dependency;

		if (vkCreateRenderPass(logicalDevice, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS)
		{
			TNG_ASSERT_MSG(false, "Failed to create render pass!");
		}


	}

	void Renderer::CreateFramebuffers()
	{
		auto& swidd = swapChainImageDependentData;

		for (size_t i = 0; i < GetSWIDDSize(); i++)
		{
			std::array<VkImageView, 3> attachments =
			{
				colorAttachment.GetImageView(),
				depthBuffer.GetImageView(),
				swidd[i].swapChainImage.GetImageView()
			};

			VkFramebufferCreateInfo framebufferInfo{};
			framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			framebufferInfo.renderPass = renderPass;
			framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			framebufferInfo.pAttachments = attachments.data();
			framebufferInfo.width = swapChainExtent.width;
			framebufferInfo.height = swapChainExtent.height;
			framebufferInfo.layers = 1;

			if (vkCreateFramebuffer(logicalDevice, &framebufferInfo, nullptr, &(swidd[i].swapChainFramebuffer)) != VK_SUCCESS)
			{
				TNG_ASSERT_MSG(false, "Failed to create framebuffer!");
			}
		}
	}

	void Renderer::CreatePrimaryCommandBuffers(QueueType poolType)
	{
		for (uint32_t i = 0; i < GetFDDSize(); i++)
		{
			GetFDDAtIndex(i)->primaryCommandBuffer.Create(logicalDevice, CommandPoolRegistry::GetInstance().GetCommandPool(poolType));
		}
	}

	void Renderer::CreateSyncObjects()
	{
		VkSemaphoreCreateInfo semaphoreInfo{};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		// Creates the fence on the signaled state so we don't block on this fence for
		// the first frame (when we don't have any previous frames to wait on)
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		for (uint32_t i = 0; i < GetFDDSize(); i++)
		{
			if (vkCreateSemaphore(logicalDevice, &semaphoreInfo, nullptr, &(GetFDDAtIndex(i)->imageAvailableSemaphore)) != VK_SUCCESS
				|| vkCreateSemaphore(logicalDevice, &semaphoreInfo, nullptr, &(GetFDDAtIndex(i)->renderFinishedSemaphore)) != VK_SUCCESS
				|| vkCreateFence(logicalDevice, &fenceInfo, nullptr, &(GetFDDAtIndex(i)->inFlightFence)) != VK_SUCCESS)
			{
				TNG_ASSERT_MSG(false, "Failed to create semaphores or fences!");
			}
		}
	}

	void Renderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory)
	{
		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = size;
		bufferInfo.usage = usage;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (vkCreateBuffer(logicalDevice, &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
		{
			TNG_ASSERT_MSG(false, "Failed to create buffer!");
		}

		VkMemoryRequirements memRequirements;
		vkGetBufferMemoryRequirements(logicalDevice, buffer, &memRequirements);

		VkMemoryAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memRequirements.size;
		allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, properties);

		if (vkAllocateMemory(logicalDevice, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS)
		{
			TNG_ASSERT_MSG(false, "Failed to allocate memory for the buffer!");
		}

		vkBindBufferMemory(logicalDevice, buffer, bufferMemory, 0);
	}

	void Renderer::CreateAssetUniformBuffers(UUID uuid)
	{
		VkDeviceSize transformUBOSize = sizeof(TransformUBO);
		VkDeviceSize viewUBOSize = sizeof(ViewUBO);
		VkDeviceSize projUBOSize = sizeof(ProjUBO);
		VkDeviceSize cameraDataSize = sizeof(CameraDataUBO);

		for (uint32_t i = 0; i < GetFDDSize(); i++)
		{
			FrameDependentData* currentFDD = GetFDDAtIndex(i);
			AssetDescriptorData& assetDescriptorData = currentFDD->assetDescriptorDataMap[uuid];

			// Create the TransformUBO
			UniformBuffer& transUBO = assetDescriptorData.transformUBO;
			transUBO.Create(physicalDevice, logicalDevice, transformUBOSize);
			transUBO.MapMemory(logicalDevice, transformUBOSize);
				
			// Create the ViewUBO
			UniformBuffer& vpUBO = assetDescriptorData.viewUBO;
			vpUBO.Create(physicalDevice, logicalDevice, viewUBOSize);
			vpUBO.MapMemory(logicalDevice, viewUBOSize);

			// Create the ProjUBO
			UniformBuffer& projUBO = assetDescriptorData.projUBO;
			projUBO.Create(physicalDevice, logicalDevice, projUBOSize);
			projUBO.MapMemory(logicalDevice, projUBOSize);

			// Create the camera data UBO
			UniformBuffer& cameraDataUBO = assetDescriptorData.cameraDataUBO;
			cameraDataUBO.Create(physicalDevice, logicalDevice, cameraDataSize);
			cameraDataUBO.MapMemory(logicalDevice, cameraDataSize);
		}
	}

	void Renderer::CreateDescriptorSetLayouts()
	{
		//	DIFFUSE = 0,
		//	NORMAL,
		//	METALLIC,
		//	ROUGHNESS,
		//	LIGHTMAP,

		// Holds PBR textures
		SetLayoutSummary persistentLayout;
		persistentLayout.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // Diffuse texture
		persistentLayout.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // Normal texture
		persistentLayout.AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // Metallic texture
		persistentLayout.AddBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // Roughness texture
		persistentLayout.AddBinding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // Lightmap texture
		setLayoutCache.CreateSetLayout(logicalDevice, persistentLayout, 0);

		// Holds ProjUBO
		SetLayoutSummary unstableLayout;
		unstableLayout.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);           // Projection matrix
		unstableLayout.AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);         // Camera exposure
		setLayoutCache.CreateSetLayout(logicalDevice, unstableLayout, 0);

		// Holds TransformUBO + ViewUBO + CameraDataUBO
		SetLayoutSummary volatileLayout;
		volatileLayout.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);   // Transform matrix
		volatileLayout.AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT); // Camera data
		volatileLayout.AddBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);   // View matrix
		setLayoutCache.CreateSetLayout(logicalDevice, volatileLayout, 0);
	}

	void Renderer::CreateDescriptorPool()
	{
		// We will create a descriptor pool that can allocate a large number of descriptor sets using the following logic:
		// Since we have to allocate a descriptor set for every unique asset (not sure if this is the correct way, to be honest)
		// and for every frame in flight, we'll set a maximum number of assets (100) and multiply that by the max number of frames
		// in flight
		// TODO - Once I learn how to properly set a different transform for every asset, this will probably have to change...I just
		//        don't know what I'm doing.
		const uint32_t fddSize = GetFDDSize();

		const uint32_t numUniformBuffers = 4;
		const uint32_t numImageSamplers = 5;

		std::array<VkDescriptorPoolSize, 2> poolSizes{};
		poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		poolSizes[0].descriptorCount = numUniformBuffers * GetFDDSize();
		poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSizes[1].descriptorCount = numImageSamplers * GetFDDSize();

		descriptorPool.Create(logicalDevice, poolSizes.data(), static_cast<uint32_t>(poolSizes.size()), static_cast<uint32_t>(poolSizes.size()) * fddSize * MAX_ASSET_COUNT, 0);
	}

	void Renderer::CreateDescriptorSets(UUID uuid)
	{
		uint32_t fddSize = GetFDDSize();

		for (uint32_t i = 0; i < fddSize; i++)
		{
			FrameDependentData* currentFDD = GetFDDAtIndex(i);
			currentFDD->assetDescriptorDataMap.insert({uuid, AssetDescriptorData()});
			AssetDescriptorData& assetDescriptorData = currentFDD->assetDescriptorDataMap[uuid];

			LayoutCache& cache = setLayoutCache.GetLayoutCache();
			for (auto iter : cache)
			{
				assetDescriptorData.descriptorSets.push_back(DescriptorSet());
				DescriptorSet* currentSet = &assetDescriptorData.descriptorSets.back();

				currentSet->Create(logicalDevice, descriptorPool, iter.second);
			}
		}

	}

	void Renderer::CreateDepthTexture()
	{
		VkFormat depthFormat = FindDepthFormat();

		// Base image
		BaseImageCreateInfo imageInfo{};
		imageInfo.width = framebufferWidth;
		imageInfo.height = framebufferHeight;
		imageInfo.format = depthFormat;
		imageInfo.format = depthFormat;
		imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		imageInfo.mipLevels = 1;
		imageInfo.samples = msaaSamples;

		// Image view
		ImageViewCreateInfo imageViewInfo{ VK_IMAGE_ASPECT_DEPTH_BIT };

		depthBuffer.Create(physicalDevice, logicalDevice, &imageInfo, &imageViewInfo);
	}

	void Renderer::CreateColorAttachmentTexture()
	{
		// Base image
		BaseImageCreateInfo imageInfo{};
		imageInfo.width = framebufferWidth;
		imageInfo.height = framebufferHeight;
		imageInfo.format = swapChainImageFormat;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		imageInfo.mipLevels = 1;
		imageInfo.samples = msaaSamples;

		// Image view
		ImageViewCreateInfo imageViewInfo{ VK_IMAGE_ASPECT_COLOR_BIT };

		colorAttachment.Create(physicalDevice, logicalDevice, &imageInfo, &imageViewInfo);
	}

	void Renderer::RecordPrimaryCommandBuffer(uint32_t frameBufferIndex)
	{
		PrimaryCommandBuffer* commandBuffer = GetCurrentPrimaryBuffer();

		// Reset the primary buffer since we've marked it as one-time submit
		commandBuffer->Reset();

		// Primary command buffers don't need to define inheritance info
		commandBuffer->BeginRecording(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr);

		commandBuffer->CMD_BeginRenderPass(renderPass, GetFramebufferAtIndex(frameBufferIndex), swapChainExtent, true);

		// Execute the secondary commands here
		std::vector<VkCommandBuffer> secondaryCmdBuffers;
		secondaryCmdBuffers.resize(assetResources.size()); // At most we can have the same number of cmd buffers as there are asset resources
		uint32_t secondaryCmdBufferCount = 0;
		for (auto& iter : assetResources)
		{
			UUID& uuid = iter.uuid;

			if (assetResources[resourcesMap[uuid]].shouldDraw)
			{
				SecondaryCommandBuffer* secondaryCmdBuffer = GetSecondaryCommandBufferAtIndex(frameBufferIndex, uuid);

				UpdateTransformUniformBuffer(assetResources[resourcesMap[uuid]].transform, uuid);
				UpdateTransformDescriptorSet(uuid);

				secondaryCmdBuffer->Reset();
				RecordSecondaryCommandBuffer(*secondaryCmdBuffer, &iter, frameBufferIndex);

				secondaryCmdBuffers[secondaryCmdBufferCount++] = secondaryCmdBuffer->GetBuffer();
			}
		}

		// Don't attempt to execute 0 command buffers
		if (secondaryCmdBufferCount > 0)
		{
			commandBuffer->CMD_ExecuteSecondaryCommands(secondaryCmdBuffers.data(), secondaryCmdBufferCount);
		}

		commandBuffer->CMD_EndRenderPass();

		commandBuffer->EndRecording();
	}

	VkShaderModule Renderer::LoadShader(const std::string& fileName)
	{
		namespace fs = std::filesystem;
		const std::string defaultShaderCompiledPath = (fs::path(compiledShaderOutputPath) / fs::path(fileName)).generic_string();

		auto shaderCode = ReadFile(defaultShaderCompiledPath);
		return CreateShaderModule(shaderCode.data(), static_cast<uint32_t>(shaderCode.size()));
	}

	void Renderer::RecordSecondaryCommandBuffer(SecondaryCommandBuffer& commandBuffer, AssetResources* resources, uint32_t frameBufferIndex)
	{
		// Retrieve the vector of descriptor sets for the given asset
		auto& descSets = GetCurrentFDD()->assetDescriptorDataMap[resources->uuid].descriptorSets;
		std::vector<VkDescriptorSet> vkDescSets(descSets.size());
		for (uint32_t i = 0; i < descSets.size(); i++)
		{
			vkDescSets[i] = descSets[i].GetDescriptorSet();
		}

		VkCommandBufferInheritanceInfo inheritanceInfo{};
		inheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
		inheritanceInfo.pNext = nullptr;
		inheritanceInfo.renderPass = renderPass; // NOTE - We only have one render pass for now, if that changes we must change it here too
		inheritanceInfo.subpass = 0;
		inheritanceInfo.framebuffer = GetSWIDDAtIndex(frameBufferIndex)->swapChainFramebuffer;

		commandBuffer.BeginRecording(VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT, &inheritanceInfo);

		commandBuffer.CMD_BindMesh(resources);
		commandBuffer.CMD_BindDescriptorSets(pipelineLayout, static_cast<uint32_t>(vkDescSets.size()), vkDescSets.data());
		commandBuffer.CMD_BindGraphicsPipeline(graphicsPipeline);
		commandBuffer.CMD_SetScissor({ 0, 0 }, swapChainExtent);
		commandBuffer.CMD_SetViewport(static_cast<float>(swapChainExtent.width), static_cast<float>(swapChainExtent.height));
		commandBuffer.CMD_DrawIndexed(static_cast<uint32_t>(resources->indexCount));

		commandBuffer.EndRecording();
	}

	void Renderer::RecreateAllSecondaryCommandBuffers()
	{
		for (uint32_t i = 0; i < GetSWIDDSize(); i++)
		{
			for (auto& resources : assetResources)
			{
				SecondaryCommandBuffer* commandBuffer = GetSecondaryCommandBufferAtIndex(i, resources.uuid);
				commandBuffer->Create(logicalDevice, CommandPoolRegistry::GetInstance().GetCommandPool(QueueType::GRAPHICS));
				RecordSecondaryCommandBuffer(*commandBuffer, &resources, i);
			}
		}
	}

	void Renderer::CleanupSwapChain()
	{
		colorAttachment.Destroy(logicalDevice);
		depthBuffer.Destroy(logicalDevice);

		for (auto& swidd : swapChainImageDependentData)
		{
			vkDestroyFramebuffer(logicalDevice, swidd.swapChainFramebuffer, nullptr);
		}

		for (auto& swidd : swapChainImageDependentData)
		{
			swidd.swapChainImage.DestroyImageView(logicalDevice);
		}

		// Clean up the secondary commands buffers that reference the swap chain framebuffers
		for (uint32_t i = 0; i < GetSWIDDSize(); i++)
		{
			auto& secondaryCmdBuffer = swapChainImageDependentData[i].secondaryCommandBuffer;
			for (auto iter : secondaryCmdBuffer)
			{
				iter.second.Destroy(logicalDevice, CommandPoolRegistry::GetInstance().GetCommandPool(QueueType::GRAPHICS));
			}
		}

		vkDestroySwapchainKHR(logicalDevice, swapChain, nullptr);
	}

	void Renderer::UpdateProjectionUniformBuffer(UUID uuid, uint32_t frameIndex)
	{
		using namespace glm;

		float aspectRatio = swapChainExtent.width / static_cast<float>(swapChainExtent.height);

		// Construct the ProjUBO
		ProjUBO projUBO;
		projUBO.proj = perspective(radians(45.0f), aspectRatio, 0.1f, 1000.0f);

		// NOTE - GLM was originally designed for OpenGL, where the Y coordinate of the clip coordinates is inverted
		projUBO.proj[1][1] *= -1;

		FrameDependentData* currentFDD = GetFDDAtIndex(frameIndex);
		auto& currentAssetDataMap = currentFDD->assetDescriptorDataMap[uuid];

		currentAssetDataMap.projUBO.UpdateData(&projUBO, sizeof(ProjUBO));
	}

	void Renderer::UpdateProjectionDescriptorSet(UUID uuid, uint32_t frameIndex)
	{
		FrameDependentData* currentFDD = GetFDDAtIndex(frameIndex);
		auto& currentAssetDataMap = currentFDD->assetDescriptorDataMap[uuid];

		DescriptorSet& descSet = currentAssetDataMap.descriptorSets[1];

		// Update ProjUBO descriptor set
		WriteDescriptorSets writeDescSets(1, 0);
		writeDescSets.AddUniformBuffer(descSet.GetDescriptorSet(), 0, currentAssetDataMap.projUBO.GetBuffer(), currentAssetDataMap.projUBO.GetBufferSize(), 0);
		descSet.Update(logicalDevice, writeDescSets);
	}

	void Renderer::UpdatePBRTextureDescriptorSet(UUID uuid, uint32_t frameIndex)
	{
		FrameDependentData* currentFDD = GetFDDAtIndex(frameIndex);
		auto& currentAssetDataMap = currentFDD->assetDescriptorDataMap[uuid];

		DescriptorSet& descSet = currentAssetDataMap.descriptorSets[0];

		// Get the asset resources so we can retrieve the textures
		AssetResources& resources = assetResources[resourcesMap[uuid]];

		// Update PBR textures
		WriteDescriptorSets writeDescSets(0, 5);
		writeDescSets.AddImageSampler(descSet.GetDescriptorSet(), 0, resources.material[static_cast<uint32_t>(Material::TEXTURE_TYPE::DIFFUSE)]);
		writeDescSets.AddImageSampler(descSet.GetDescriptorSet(), 1, resources.material[static_cast<uint32_t>(Material::TEXTURE_TYPE::NORMAL)]);
		writeDescSets.AddImageSampler(descSet.GetDescriptorSet(), 2, resources.material[static_cast<uint32_t>(Material::TEXTURE_TYPE::METALLIC)]);
		writeDescSets.AddImageSampler(descSet.GetDescriptorSet(), 3, resources.material[static_cast<uint32_t>(Material::TEXTURE_TYPE::ROUGHNESS)]);
		writeDescSets.AddImageSampler(descSet.GetDescriptorSet(), 4, resources.material[static_cast<uint32_t>(Material::TEXTURE_TYPE::LIGHTMAP)]);

		descSet.Update(logicalDevice, writeDescSets);
	}

	void Renderer::UpdateCameraDataDescriptorSet(UUID uuid, uint32_t frameIndex)
	{
		FrameDependentData* currentFDD = GetFDDAtIndex(frameIndex);
		auto& currentAssetDataMap = currentFDD->assetDescriptorDataMap[uuid];

		DescriptorSet& descSet = currentAssetDataMap.descriptorSets[2];

		// Update view matrix + camera data descriptor set
		WriteDescriptorSets writeDescSets(2, 0);
		writeDescSets.AddUniformBuffer(descSet.GetDescriptorSet(), 2, currentAssetDataMap.viewUBO.GetBuffer(), currentAssetDataMap.viewUBO.GetBufferSize(), 0);
		writeDescSets.AddUniformBuffer(descSet.GetDescriptorSet(), 0, currentAssetDataMap.cameraDataUBO.GetBuffer(), currentAssetDataMap.cameraDataUBO.GetBufferSize(), 0);
		descSet.Update(logicalDevice, writeDescSets);
	}

	void Renderer::UpdateTransformUniformBuffer(const Transform& transform, UUID uuid)
	{
		// Construct and update the transform UBO
		TransformUBO tempUBO{};

		glm::mat4 finalTransform = glm::identity<glm::mat4>();

		glm::mat4 translation = glm::translate(glm::identity<glm::mat4>(), transform.position);
		glm::mat4 rotation = glm::eulerAngleXYZ(transform.rotation.x, transform.rotation.y, transform.rotation.z);
		glm::mat4 scale = glm::scale(glm::identity<glm::mat4>(), transform.scale);

		tempUBO.transform = translation * rotation * scale;
		GetCurrentFDD()->assetDescriptorDataMap[uuid].transformUBO.UpdateData(&tempUBO, sizeof(TransformUBO));
	}

	void Renderer::UpdateCameraDataUniformBuffers(UUID uuid, uint32_t frameIndex, const glm::vec3& position, const glm::mat4& viewMatrix)
	{
		FrameDependentData* currentFDD = GetFDDAtIndex(frameIndex);
		auto& assetDescriptorData = currentFDD->assetDescriptorDataMap[uuid];

		ViewUBO viewUBO{};
		viewUBO.view = viewMatrix;
		assetDescriptorData.viewUBO.UpdateData(&viewUBO, sizeof(ViewUBO));

		CameraDataUBO cameraDataUBO{};
		cameraDataUBO.position = glm::vec4(position, 1.0f);
		cameraDataUBO.exposure = 1.0f;
		assetDescriptorData.cameraDataUBO.UpdateData(&cameraDataUBO, sizeof(CameraDataUBO));
	}

	void Renderer::UpdateTransformDescriptorSet(UUID uuid)
	{
		FrameDependentData* currentFDD = GetCurrentFDD();
		auto& currentAssetDataMap = currentFDD->assetDescriptorDataMap[uuid];

		DescriptorSet& descSet = currentAssetDataMap.descriptorSets[2];

		// Update transform + cameraData descriptor sets
		WriteDescriptorSets writeDescSets(2, 0);
		writeDescSets.AddUniformBuffer(descSet.GetDescriptorSet(), 0, currentAssetDataMap.transformUBO.GetBuffer(), currentAssetDataMap.transformUBO.GetBufferSize(), 0);
		writeDescSets.AddUniformBuffer(descSet.GetDescriptorSet(), 1, currentAssetDataMap.cameraDataUBO.GetBuffer(), currentAssetDataMap.cameraDataUBO.GetBufferSize(), 0);
		descSet.Update(logicalDevice, writeDescSets);
	}

	void Renderer::InitializeDescriptorSets(UUID uuid, uint32_t frameIndex)
	{
		// Update all descriptor sets
		UpdateCameraDataDescriptorSet(uuid, frameIndex);
		UpdateProjectionDescriptorSet(uuid, frameIndex);
		UpdatePBRTextureDescriptorSet(uuid, frameIndex);
	}

	VkResult Renderer::SubmitQueue(QueueType type, VkSubmitInfo* info, uint32_t submitCount, VkFence fence, bool waitUntilIdle)
	{
		VkResult res;
		VkQueue queue = queues[type];

		res = vkQueueSubmit(queue, submitCount, info, fence);
		if (res != VK_SUCCESS)
		{
			TNG_ASSERT_MSG(false, "Failed to submit queue!");
		}

		if (waitUntilIdle)
		{
			res = vkQueueWaitIdle(queue);
			if (res != VK_SUCCESS)
			{
				TNG_ASSERT_MSG(false, "Failed to wait until queue was idle after submitting!");
			}
		}

		return res;
	}

	void Renderer::CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height)
	{
		DisposableCommand command(logicalDevice, QueueType::TRANSFER);

		VkBufferImageCopy region{};
		region.bufferOffset = 0;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;

		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;

		region.imageOffset = { 0, 0, 0 };
		region.imageExtent = { width, height, 1 };

		vkCmdCopyBufferToImage(command.GetBuffer(), buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}

	VkFormat Renderer::FindSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features)
	{
		for (VkFormat format : candidates)
		{
			VkFormatProperties props;
			vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);

			if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
			{
				return format;
			}
			else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features)
			{
				return format;
			}
		}

		TNG_ASSERT_MSG(false, "Failed to find supported format!");
		return VK_FORMAT_UNDEFINED;
	}

	VkFormat Renderer::FindDepthFormat()
	{
		return FindSupportedFormat(
			{ VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT },
			VK_IMAGE_TILING_OPTIMAL,
			VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
		);
	}

	bool Renderer::HasStencilComponent(VkFormat format)
	{
		return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
	}

	VkSampleCountFlagBits Renderer::GetMaxUsableSampleCount()
	{
		VkPhysicalDeviceProperties physicalDeviceProperties;
		vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);

		VkSampleCountFlags counts = physicalDeviceProperties.limits.framebufferColorSampleCounts & physicalDeviceProperties.limits.framebufferDepthSampleCounts;
		if (counts & VK_SAMPLE_COUNT_64_BIT) { return VK_SAMPLE_COUNT_64_BIT; }
		if (counts & VK_SAMPLE_COUNT_32_BIT) { return VK_SAMPLE_COUNT_32_BIT; }
		if (counts & VK_SAMPLE_COUNT_16_BIT) { return VK_SAMPLE_COUNT_16_BIT; }
		if (counts & VK_SAMPLE_COUNT_8_BIT) { return VK_SAMPLE_COUNT_8_BIT; }
		if (counts & VK_SAMPLE_COUNT_4_BIT) { return VK_SAMPLE_COUNT_4_BIT; }
		if (counts & VK_SAMPLE_COUNT_2_BIT) { return VK_SAMPLE_COUNT_2_BIT; }

		return VK_SAMPLE_COUNT_1_BIT;
	}

	/////////////////////////////////////////////////////
	// 
	// Frame dependent data
	//
	/////////////////////////////////////////////////////
	Renderer::FrameDependentData* Renderer::GetCurrentFDD()
	{
		return &frameDependentData[currentFrame];
	}

	Renderer::FrameDependentData* Renderer::GetFDDAtIndex(uint32_t frameIndex)
	{
		TNG_ASSERT_MSG(frameIndex >= 0 && frameIndex < frameDependentData.size(), "Invalid index used to retrieve frame-dependent data");
		return &frameDependentData[frameIndex];
	}

	uint32_t Renderer::GetFDDSize() const
	{
		return static_cast<uint32_t>(frameDependentData.size());
	}

	/////////////////////////////////////////////////////
	// 
	// Swap-chain image dependent data
	//
	/////////////////////////////////////////////////////

	// Returns the swap-chain image dependent data at the provided index
	Renderer::SwapChainImageDependentData* Renderer::GetSWIDDAtIndex(uint32_t frameIndex)
	{
		TNG_ASSERT_MSG(frameIndex >= 0 && frameIndex < swapChainImageDependentData.size(), "Invalid index used to retrieve swap-chain image dependent data");
		return &swapChainImageDependentData[frameIndex];
	}

	uint32_t Renderer::GetSWIDDSize() const
	{
		return static_cast<uint32_t>(swapChainImageDependentData.size());
	}

}