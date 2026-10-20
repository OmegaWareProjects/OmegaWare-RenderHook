#include "OmegaRenderHook/Core/Log.hpp"

#include <Windows.h>

#include <mutex>
#include <string>

namespace OmegaRenderHook
{
	namespace
	{
		std::mutex s_mtxSink;
		LogSink s_fnSink;
	}

	void SetLogSink(LogSink fnSink)
	{
		std::scoped_lock lckSink(s_mtxSink);
		s_fnSink = std::move(fnSink);
	}

	void LogMessage(ELogLevel eLevel, std::string_view strMessage)
	{
		std::scoped_lock lckSink(s_mtxSink);
		if (s_fnSink)
		{
			s_fnSink(eLevel, strMessage);
			return;
		}
		const std::string strLine = std::format("[OmegaRenderHook] [{}] {}\n", ToString(eLevel), strMessage);
		OutputDebugStringA(strLine.c_str());
	}
}
