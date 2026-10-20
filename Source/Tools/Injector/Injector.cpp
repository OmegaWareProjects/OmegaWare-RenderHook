#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace
{
	class clHandle
	{
	public:
		explicit clHandle(HANDLE hHandle = nullptr) : m_hHandle(hHandle) {}
		~clHandle()
		{
			if (m_hHandle && m_hHandle != INVALID_HANDLE_VALUE)
				CloseHandle(m_hHandle);
		}

		clHandle(const clHandle&) = delete;
		clHandle& operator=(const clHandle&) = delete;

		[[nodiscard]] HANDLE Get() const { return m_hHandle; }
		explicit operator bool() const { return m_hHandle && m_hHandle != INVALID_HANDLE_VALUE; }

	private:
		HANDLE m_hHandle;
	};

	[[nodiscard]] std::optional<DWORD> FindProcess(std::wstring_view strTarget)
	{
		if (!strTarget.empty() && std::ranges::all_of(strTarget, [](wchar_t chDigit) { return chDigit >= L'0' && chDigit <= L'9'; }))
			return static_cast<DWORD>(std::wcstoul(std::wstring(strTarget).c_str(), nullptr, 10));

		clHandle hSnapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
		if (!hSnapshot)
			return std::nullopt;

		PROCESSENTRY32W stEntry{ sizeof(PROCESSENTRY32W) };
		for (BOOL bMore = Process32FirstW(hSnapshot.Get(), &stEntry); bMore; bMore = Process32NextW(hSnapshot.Get(), &stEntry))
		{
			if (_wcsicmp(stEntry.szExeFile, std::wstring(strTarget).c_str()) == 0)
				return stEntry.th32ProcessID;
		}
		return std::nullopt;
	}

	[[nodiscard]] USHORT ProcessMachine(HANDLE hProcess)
	{
		USHORT uProcess = IMAGE_FILE_MACHINE_UNKNOWN;
		USHORT uNative = IMAGE_FILE_MACHINE_UNKNOWN;
		if (!IsWow64Process2(hProcess, &uProcess, &uNative))
			return IMAGE_FILE_MACHINE_UNKNOWN;
		return uProcess == IMAGE_FILE_MACHINE_UNKNOWN ? uNative : uProcess;
	}
}

int wmain(int iArgCount, wchar_t** pszArgs)
{
	if (iArgCount < 3)
	{
		std::fwprintf(stderr, L"usage: OmegaRenderHook-Injector.exe <process name | pid> <dll path>\n");
		return 1;
	}

	const std::optional<DWORD> optProcessId = FindProcess(pszArgs[1]);
	if (!optProcessId)
	{
		std::fwprintf(stderr, L"process %ls not found\n", pszArgs[1]);
		return 1;
	}

	std::error_code ecPath;
	const std::filesystem::path pathModule = std::filesystem::absolute(pszArgs[2], ecPath);
	if (ecPath || !std::filesystem::exists(pathModule))
	{
		std::fwprintf(stderr, L"module %ls not found\n", pszArgs[2]);
		return 1;
	}

	clHandle hProcess(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, *optProcessId));
	if (!hProcess)
	{
		std::fwprintf(stderr, L"OpenProcess(%lu) failed: %lu\n", *optProcessId, GetLastError());
		return 1;
	}

	if (ProcessMachine(hProcess.Get()) != ProcessMachine(GetCurrentProcess()))
	{
		std::fwprintf(stderr, L"architecture mismatch, use the injector built for the target's architecture\n");
		return 1;
	}

	const std::wstring strModule = pathModule.wstring();
	const SIZE_T uBytes = (strModule.size() + 1) * sizeof(wchar_t);
	void* pRemote = VirtualAllocEx(hProcess.Get(), nullptr, uBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!pRemote)
	{
		std::fwprintf(stderr, L"VirtualAllocEx failed: %lu\n", GetLastError());
		return 1;
	}

	int iResult = 1;
	auto* pfnLoadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
	if (WriteProcessMemory(hProcess.Get(), pRemote, strModule.c_str(), uBytes, nullptr))
	{
		clHandle hThread(CreateRemoteThread(hProcess.Get(), nullptr, 0, pfnLoadLibrary, pRemote, 0, nullptr));
		if (!hThread)
			std::fwprintf(stderr, L"CreateRemoteThread failed: %lu\n", GetLastError());
		else if (WaitForSingleObject(hThread.Get(), 15000) != WAIT_OBJECT_0)
			std::fwprintf(stderr, L"LoadLibraryW did not return within 15 seconds\n");
		else
		{
			DWORD uExitCode = 0;
			GetExitCodeThread(hThread.Get(), &uExitCode);
			if (uExitCode == 0)
				std::fwprintf(stderr, L"LoadLibraryW failed inside the target\n");
			else
			{
				std::wprintf(L"injected %ls into process %lu\n", strModule.c_str(), *optProcessId);
				iResult = 0;
			}
		}
	}
	else
		std::fwprintf(stderr, L"WriteProcessMemory failed: %lu\n", GetLastError());

	VirtualFreeEx(hProcess.Get(), pRemote, 0, MEM_RELEASE);
	return iResult;
}
