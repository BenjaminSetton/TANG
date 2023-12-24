#ifndef CONFIG_H
#define CONFIG_H

#include <string>

namespace TANG
{
	namespace CONFIG
	{
		static constexpr uint32_t WindowWidth = 1280;
		static constexpr uint32_t WindowHeight = 720;

		static const std::string SkyboxCubeMeshFilePath = "../src/data/assets/cube.fbx";

		static constexpr uint32_t MaxFramesInFlight = 2;
		static constexpr uint32_t MaxAssetCount = 100;
	}
}

#endif