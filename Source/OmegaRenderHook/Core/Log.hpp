#pragma once

#include <format>
#include <functional>
#include <string_view>
#include <utility>

namespace OmegaRenderHook
{
	enum class ELogLevel
	{
		Debug,
		Info,
		Warning,
		Error
	};

	using LogSink = std::function<void(ELogLevel, std::string_view)>;

	void SetLogSink(LogSink fnSink);
	void LogMessage(ELogLevel eLevel, std::string_view strMessage);

	[[nodiscard]] constexpr std::string_view ToString(ELogLevel eLevel)
	{
		switch (eLevel)
		{
		case ELogLevel::Debug: return "debug";
		case ELogLevel::Info: return "info";
		case ELogLevel::Warning: return "warning";
		case ELogLevel::Error: return "error";
		}
		return "?";
	}

	template <typename... Args>
	void Log(ELogLevel eLevel, std::format_string<Args...> fmtMessage, Args&&... args)
	{
		LogMessage(eLevel, std::format(fmtMessage, std::forward<Args>(args)...));
	}
}
