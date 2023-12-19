
#include "base_pipeline.h"

namespace TANG
{
	class PBRPipeline : public BasePipeline
	{
	public:

		void Create(VkRenderPass renderPass, const SetLayoutCache& setLayoutCache, const VkExtent2D& viewportSize) override;

	};
}