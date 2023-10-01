#ifndef RENDERER_H
#define RENDERER_H

//
// This code was taken from the Vulkan tutorial website:
// https://vulkan-tutorial.com
//

#define VK_USE_PLATFORM_WIN32_KHR
#define GLFW_INCLUDE_VULKAN
#include <glfw3.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <glfw3native.h>

#include <unordered_map>
#include <vector>

#include "asset_types.h"
#include "cmd_buffer/primary_command_buffer.h"
#include "cmd_buffer/secondary_command_buffer.h"
#include "data_buffer/uniform_buffer.h"
#include "descriptors/descriptor_pool.h"
#include "descriptors/descriptors.h"
#include "utils/sanity_check.h"

namespace TANG
{
	struct QueueFamilyIndices;
	struct SwapChainSupportDetails;

	enum QueueType
	{
		GRAPHICS_QUEUE,
		PRESENT_QUEUE,
		TRANSFER_QUEUE,
		QUEUE_COUNT			// NOTE! This value must come last at all times!! This is used to count the number of values inside this enum
	};

	class Renderer {

	public:

		void Initialize();

		// Receives an OPTIONAL parameter of deltaTime. Since this renderer is meant to work with the API, we'll give the user the option
		// of passing in a deltaTime to the renderer. If nullptr is passed in, we'll calculate it instead
		void Update(float* deltaTime);

		// The core draw call. Conventionally, the state of the renderer must be updated through a call to Update() before this call is made
		void Draw();

		// Releases all internal handles to Vulkan objects
		void Shutdown();

		// Sets the draw state of the given asset to TRUE.
		// The asset draw state is cleared every frame, so SetAssetDrawState() must be called on a
		// per-frame basis to draw assets. In other words, assets will not be drawn unless SetAssetDrawState()
		// is explicitly called that frame.
		// NOTE - No getter is defined on purpose, the data should only be received from the API and kept in the renderer
		void SetAssetDrawState(UUID uuid);

		// The following functions provide different ways of modifying the internal transform data of the provided asset
		// NOTE - No getters are defined on purpose, the data should only be received from the API and kept in the renderer
		void SetAssetTransform(UUID uuid, Transform& transform);
		void SetAssetPosition(UUID uuid, glm::vec3& position);
		void SetAssetRotation(UUID uuid, glm::vec3& rotation);
		void SetAssetScale(UUID uuid, glm::vec3& scale);

		// Loads an asset which implies grabbing the vertices and indices from the asset container
		// and creating vertex/index buffers to contain them. It also includes creating all other
		// API objects necessary for rendering.
		// 
		// Before calling this function, make sure you've called LoaderUtils::LoadAsset() and have
		// successfully loaded an asset from file! This functions assumes this, and if it can't retrieve
		// the loaded asset data it will return prematurely
		AssetResources* CreateAssetResources(AssetDisk* asset);

		// Creates a secondary command buffer, given the asset resources. After an asset is loaded and it's asset resources
		// are loaded, this function must be called to create the secondary command buffer that holds the commands to render
		// the asset.
		void CreateAssetCommandBuffer(AssetResources* resources);

		void DestroyAssetResources(AssetDisk* asset);
		void DestroyAllAssetResources();

		// TODO - Abstract this out into a window class, this really shouldn't be part of the renderer
		bool WindowShouldClose();

		bool framebufferResized = false;

	private:

		static void framebufferResizeCallback(GLFWwindow* windowHandle, int width, int height)
		{
			UNUSED(width);
			UNUSED(height);

			auto app = reinterpret_cast<Renderer*>(glfwGetWindowUserPointer(windowHandle));
			app->framebufferResized = true;
		}

		// NOTE - The window creation and management using GLFW will be abstracted away from this class in the future.
		//        The renderer should only be in charge of initializing, maintaining and destroying Vulkan-related objects.
		void InitWindow();
		void InitVulkan();

		void ShutdownWindow();
		void ShutdownVulkan();

		void DrawFrame();

		void CreateInstance();

		void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo);

		void SetupDebugMessenger();

		std::vector<const char*> GetRequiredExtensions();

		bool CheckValidationLayerSupport();

		bool CheckDeviceExtensionSupport(VkPhysicalDevice device);

		////////////////////////////////////////
		//
		//  PHYSICAL DEVICE
		//
		////////////////////////////////////////
		void PickPhysicalDevice();

		bool IsDeviceSuitable(VkPhysicalDevice device);

		QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);

		SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device);

		VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);

		VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);

		VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);

		uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

		////////////////////////////////////////
		//
		//  LOGICAL DEVICE
		//
		////////////////////////////////////////
		void CreateLogicalDevice();

		////////////////////////////////////////
		//
		//  SURFACE
		//
		////////////////////////////////////////
		void CreateSurface();

		void CreateSwapChain();

		// Create image views for all images on the swap chain
		void CreateImageViews();

		// This is a helper function for creating the "VkShaderModule" wrappers around
		// the shader code, read from createGraphicsPipeline() below
		VkShaderModule CreateShaderModule(std::vector<char>& code);

		void CreateGraphicsPipeline();

		void CreateRenderPass();

		void CreateFramebuffers();

		void CreateCommandPools();

		void CreatePrimaryCommandBuffers(QueueType poolType);

		void CreateSyncObjects();

		void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory);

		void CreateUniformBuffers();

		void CreateImage(uint32_t width, uint32_t height, uint32_t mipLevels, VkSampleCountFlagBits numSamples, VkFormat format,
			VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
			VkImage& image, VkDeviceMemory& imageMemory);

		void CreateTextureImage();

		void CreateDescriptorPool();

		void CreateDescriptorBundles();

		VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, uint32_t mipLevels);

		void CreateTextureImageView();

		void CreateTextureSampler();

		void CreateDepthResources();

		void CreateColorResources();

		void RecordPrimaryCommandBuffer(uint32_t frameBufferIndex);
		void RecordSecondaryCommandBuffer(SecondaryCommandBuffer& commandBuffer, AssetResources* resources, uint32_t frameBufferIndex);

		void RecreateSwapChain();

		void RecreateAllSecondaryCommandBuffers();

		void CleanupSwapChain();

		// Updates the uniform buffers and descriptor sets that need to be update very infrequently. In this case, the view/proj uniform buffers and descriptor sets.
		// Note that this should also be called when the swap chain resizes!
		void UpdateInfrequentUniformBuffers(uint32_t frameIndex);
		void UpdateInfrequentDescriptorSets(uint32_t frameIndex);

		// Updates the uniform buffers and descriptor sets that are updated at least once per frame
		void UpdatePerFrameUniformBuffers(const Transform& transform);
		void UpdatePerFrameDescriptorSets();

		VkCommandBuffer BeginSingleTimeCommands(VkCommandPool pool);

		// The commandPoolType parameter must match the pool type that was used to allocate the command buffer in the corresponding BeginSingleTimeCommands() function call!
		void EndSingleTimeCommands(VkCommandBuffer commandBuffer, QueueType commandPoolType);

		void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mipLevels);

		void CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);

		VkFormat FindSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);

		VkFormat FindDepthFormat();

		bool HasStencilComponent(VkFormat format);

		void GenerateMipmaps(VkImage image, VkFormat imageFormat, int32_t texWidth, int32_t texHeight, uint32_t mipLevels);

		VkSampleCountFlagBits GetMaxUsableSampleCount();

		void DestroyAssetBuffersHelper(AssetResources& resources);

		PrimaryCommandBuffer* GetCurrentPrimaryBuffer();
		SecondaryCommandBuffer* GetSecondaryCommandBufferAtIndex(uint32_t frameBufferIndex, UUID uuid);
		VkFramebuffer GetFramebufferAtIndex(uint32_t frameBufferIndex);

	private:

		GLFWwindow* windowHandle = nullptr;

		VkInstance vkInstance;
		VkDebugUtilsMessengerEXT debugMessenger;

		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice logicalDevice;

		VkSurfaceKHR surface;

		std::unordered_map<QueueType, VkQueue> queues;

		VkSwapchainKHR swapChain;
		VkFormat swapChainImageFormat;
		VkExtent2D swapChainExtent;


		////////////////////////////////////////////////////////////////////
		// 
		//	FRAME-DEPENDENT DATA
		//
		//	Organizes data that depends on the maximum number of frames in flight
		// 
		////////////////////////////////////////////////////////////////////
		struct FrameDependentData
		{
			// Stores all the descriptor bundles (set and its layout) for a given frame
			std::vector<DescriptorBundle> descriptorBundles;

			// These two sets of UniformBuffer vector objects are sent to the shaders separately so that we can update them at different intervals
			// For instance, the transform for an asset can be updated every frame, while the view/projection most likely won't change every frame
			// and are usually updated when the window is resized.
			UniformBuffer transformUBO;
			UniformBuffer viewProjUBO;

			VkSemaphore imageAvailableSemaphore;
			VkSemaphore renderFinishedSemaphore;
			VkFence inFlightFence;

			// We need one primary command buffer per frame in flight, since we can be rendering multiple frames at the same time and
			// we want to still be able to reset and record a primary buffer
			PrimaryCommandBuffer primaryCommandBuffer;
		};
		std::vector<FrameDependentData> frameDependentData;
		// We want to organize our descriptor sets as follows:
		// 
		// FOR EVERY FRAME IN FLIGHT:
		//		Descriptor set 0:
		//			- viewProj uniform buffer	(binding 0)
		//			- image sampler				(binding 1)
		//		Descriptor set 1:
		//			- transform					(binding 0)
		// 
		// Total per frame in flight: 2 descriptor sets, 2 uniform buffers and 1 image sampler
		// Total for 2 frames in flight: 4 descriptor sets, 4 uniform buffers and 2 image samplers


		////////////////////////////////////////////////////////////////////
		// 
		//	SWAP-CHAIN IMAGE-DEPENDENT DATA
		//
		//	Organizes data that depends on the number of images in the swap chain, which may differ from the number of frames in flight
		// 
		////////////////////////////////////////////////////////////////////
		struct SwapChainImageDependentData
		{
			VkImage swapChainImage;
			VkImageView swapChainImageView;
			VkFramebuffer swapChainFramebuffer;

			std::unordered_map<UUID, SecondaryCommandBuffer> secondaryCommandBuffer;
		};
		std::vector<SwapChainImageDependentData> swapChainImageDependentData;

		VkRenderPass renderPass;
		VkPipelineLayout pipelineLayout;

		VkPipeline graphicsPipeline;

		std::unordered_map<QueueType, VkCommandPool> commandPools;

		// This map represents the drawable state of an asset given it's UUID. AssetDrawStates tells us whether the asset must
		// be drawn this frame and is therefore dependent directly on the frame in flight we're currently on.
		// NOTE - The size of this map exactly equals the number of loaded assets we have
		std::unordered_map<UUID, bool> assetDrawStates;

		// Stores the transform data for any asset given it's UUID
		// NOTE - The API user must update and keep track of the transform data for the assets,
		//        and pass it to the renderer every frame for drawing. The design decision behind
		//        this is so we can own copy of the data, rather than holding a ton of pointers 
		//        to data somewhere else which will probably be very slow
		std::unordered_map<UUID, Transform> assetTransforms;

		uint32_t currentFrame = 0;

		std::vector<AssetResources> assetResources;

		DescriptorPool descriptorPool;

		uint32_t textureMipLevels;
		VkImage textureImage;
		VkDeviceMemory textureImageMemory;
		VkImageView textureImageView;
		VkSampler textureSampler;

		VkImage depthImage;
		VkDeviceMemory depthImageMemory;
		VkImageView depthImageView;

		// Multisampled anti-aliasing
		VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
		VkImage colorImage;
		VkDeviceMemory colorImageMemory;
		VkImageView colorImageView;

		private:

		// Returns the current frame-dependent data
		FrameDependentData* GetCurrentFDD();

		// Returns the frame-dependent data at the provided index
		FrameDependentData* GetFDDAtIndex(uint32_t frameIndex);

		// Returns the size of the frame-dependent data vector. This is equivalent to MAX_FRAMES_IN_FLIGHT
		uint32_t GetFDDSize() const;

		// Returns the swap-chain image dependent data at the provided index
		SwapChainImageDependentData* GetSWIDDAtIndex(uint32_t frameIndex);

		// Returns the size of the swap-chain image-dependent data vector. This entirely depends on the number of images that are generated for the swap-chain
		uint32_t GetSWIDDSize() const;

	};

}

#endif