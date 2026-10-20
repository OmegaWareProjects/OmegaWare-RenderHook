#pragma once

namespace OmegaRenderHook
{
	enum class ERenderResult
	{
		Rendered,
		Skipped,
		Rebuild,
		DeviceLost,
		Faulted
	};
}
