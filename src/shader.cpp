
#include <filesystem>
#include <unordered_map>

#include "device_cache.h"
#include "shader.h"
#include "utils/file_utils.h"
#include "utils/sanity_check.h"
#include "utils/logger.h"

static const std::string CompiledShaderOutputPath = "./shaders";

static const std::unordered_map<TANG::ShaderType, std::string> ShaderTypeToFolderName =
{
	{ TANG::ShaderType::PBR, "pbr" },
	{ TANG::ShaderType::CUBEMAP_PREPROCESSING, "cubemap_preprocessing" },
	{ TANG::ShaderType::SKYBOX, "skybox" },
	{ TANG::ShaderType::LDR, "ldr_conversion" },
	{ TANG::ShaderType::IRRADIANCE_SAMPLING, "irradiance_sampling" }
};

static const std::unordered_map<TANG::ShaderStage, std::string> ShaderStageToFileName =
{
	{ TANG::ShaderStage::VERTEX_SHADER, "vert.spv" },
	{ TANG::ShaderStage::GEOMETRY_SHADER, "geom.spv" },
	{ TANG::ShaderStage::FRAGMENT_SHADER, "frag.spv" },
};

namespace TANG
{

	Shader::Shader(const ShaderType& type, const ShaderStage& stage) : shaderObject(VK_NULL_HANDLE)
	{
		Create(type, stage);
	}

	Shader::~Shader()
	{ 
		Destroy();
	}

	Shader::Shader(Shader&& other) noexcept : shaderObject(std::move(other.shaderObject))
	{ }

	void Shader::Create(const ShaderType& type, const ShaderStage& stage)
	{
		VkDevice logicalDevice = GetLogicalDevice();
		const std::string& fileName = ShaderStageToFileName.at(stage);

		namespace fs = std::filesystem;
		const std::string defaultShaderCompiledPath = (fs::path(CompiledShaderOutputPath) / fs::path(ShaderTypeToFolderName.at(type)) / fs::path(fileName.data())).generic_string();

		auto shaderCode = ReadFile(defaultShaderCompiledPath);
		if (shaderCode.empty())
		{
			LogError("Failed to create shader of type %u for stage %u", type, stage);
			return;
		}

		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = static_cast<uint32_t>(shaderCode.size());
		createInfo.pCode = reinterpret_cast<const uint32_t*>(shaderCode.data());

		if (vkCreateShaderModule(logicalDevice, &createInfo, nullptr, &shaderObject) != VK_SUCCESS)
		{
			TNG_ASSERT_MSG(false, "Failed to create shader!");
		}
	}

	void Shader::Destroy()
	{
		vkDestroyShaderModule(GetLogicalDevice(), shaderObject, nullptr);
	}

	VkShaderModule Shader::GetShaderObject() const
	{
		return shaderObject;
	}
}


