#ifndef COMMAND_POOL_REGISTRY_H
#define COMMAND_POOL_REGISTRY_H

#include <unordered_map>

#include "queue_types.h"
#include "vulkan/vulkan.h"

namespace TANG
{
	class CommandPoolRegistry
	{
	private:

		CommandPoolRegistry();
		CommandPoolRegistry(const CommandPoolRegistry& other) = delete;
		CommandPoolRegistry& operator=(const CommandPoolRegistry& other) = delete;

	public:

		static CommandPoolRegistry& GetInstance()
		{
			static CommandPoolRegistry instance;
			return instance;
		}

		void CreatePools(VkPhysicalDevice physicalDevice, VkDevice logicalDevice, VkSurfaceKHR surface);
		void DestroyPools(VkDevice logicalDevice);

		VkCommandPool GetCommandPool(QueueType type) const;

	private:

		std::unordered_map<QueueType, VkCommandPool> pools;

	};
}

#endif