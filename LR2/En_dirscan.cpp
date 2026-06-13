#include "En_dirscan.h"

#include "En_fileutil.h"
#include "LR2_songmanage.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32

#include <winternl.h>

// NtQueryDirectoryFile is not exposed by a public import library, so it is
// resolved from ntdll.dll at runtime. The status codes and the directory
// information layout are declared locally to avoid pulling in <ntstatus.h>,
// which clashes with <windows.h> macros.
namespace {

constexpr NTSTATUS kStatusSuccess = 0x00000000;
constexpr NTSTATUS kStatusBufferOverflow = 0x80000005;
constexpr NTSTATUS kStatusNoMoreFiles = 0x80000006;

using NtQueryDirectoryFile_t = NTSTATUS(NTAPI*)(HANDLE FileHandle,
                                                HANDLE Event,
                                                PIO_APC_ROUTINE ApcRoutine,
                                                PVOID ApcContext,
                                                PIO_STATUS_BLOCK IoStatusBlock,
                                                PVOID FileInformation,
                                                ULONG Length,
                                                FILE_INFORMATION_CLASS FileInformationClass,
                                                BOOLEAN ReturnSingleEntry,
                                                PUNICODE_STRING FileName,
                                                BOOLEAN RestartScan);

struct FILE_DIRECTORY_INFORMATION
{
	ULONG NextEntryOffset;
	ULONG FileIndex;
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER LastAccessTime;
	LARGE_INTEGER LastWriteTime;
	LARGE_INTEGER ChangeTime;
	LARGE_INTEGER EndOfFile;
	LARGE_INTEGER AllocationSize;
	ULONG FileAttributes;
	ULONG FileNameLength;
	WCHAR FileName[1];
};

// RAII wrapper so the directory handle is always closed on every exit path.
class DirectoryHandle
{
  public:
	explicit DirectoryHandle(HANDLE handle)
	  : handle(handle)
	{
	}
	~DirectoryHandle()
	{
		if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
			CloseHandle(handle);
		}
	}
	DirectoryHandle(const DirectoryHandle&) = delete;
	DirectoryHandle& operator=(const DirectoryHandle&) = delete;
	HANDLE get() const { return handle; }

  private:
	HANDLE handle;
};

std::wstring
ansiToWide(const char* ansi)
{
	const int needed = MultiByteToWideChar(CP_ACP, 0, ansi, -1, nullptr, 0);
	if (needed <= 0) {
		return std::wstring{};
	}
	std::wstring wide(static_cast<size_t>(needed - 1), L'\0');
	MultiByteToWideChar(CP_ACP, 0, ansi, -1, wide.data(), needed);
	return wide;
}

CSTR
wideToAnsi(const wchar_t* wide, int wideLength)
{
	const int needed = WideCharToMultiByte(
	  CP_ACP, 0, wide, wideLength, nullptr, 0, nullptr, nullptr);
	if (needed <= 0) {
		return CSTR("");
	}
	std::string out(static_cast<size_t>(needed), '\0');
	WideCharToMultiByte(
	  CP_ACP, 0, wide, wideLength, out.data(), needed, nullptr, nullptr);
	return CSTR(out.c_str(), needed);
}

} // namespace

std::vector<DirEntry>
EnumerateDirectory(CSTR directory)
{
	std::vector<DirEntry> entries;

	// Resolve the ntdll entry point once for the whole process.
	static const NtQueryDirectoryFile_t queryDirectoryFile = [] {
		const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		return reinterpret_cast<NtQueryDirectoryFile_t>(
		  ntdll ? GetProcAddress(ntdll, "NtQueryDirectoryFile") : nullptr);
	}();
	if (queryDirectoryFile == nullptr) {
		return entries;
	}

	std::wstring widePath = ansiToWide(directory.body);
	while (!widePath.empty() &&
	       (widePath.back() == L'\\' || widePath.back() == L'/')) {
		widePath.pop_back();
	}
	if (widePath.empty()) {
		return entries;
	}

	const DirectoryHandle dir{ CreateFileW(widePath.c_str(),
	                                       FILE_LIST_DIRECTORY,
	                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
	                                       nullptr,
	                                       OPEN_EXISTING,
	                                       FILE_FLAG_BACKUP_SEMANTICS,
	                                       nullptr) };
	if (dir.get() == INVALID_HANDLE_VALUE) {
		return entries;
	}

	constexpr ULONG bufferSize = 65536;
	const auto buffer = std::make_unique_for_overwrite<BYTE[]>(bufferSize);

	for (;;) {
		IO_STATUS_BLOCK isb{};
		const NTSTATUS status = queryDirectoryFile(dir.get(),
		                                            nullptr,
		                                            nullptr,
		                                            nullptr,
		                                            &isb,
		                                            buffer.get(),
		                                            bufferSize,
		                                            FileDirectoryInformation,
		                                            FALSE,
		                                            nullptr,
		                                            FALSE);
		if (status == kStatusNoMoreFiles) {
			break;
		}
		if (status != kStatusSuccess && status != kStatusBufferOverflow) {
			break;
		}

		const auto* info =
		  reinterpret_cast<const FILE_DIRECTORY_INFORMATION*>(buffer.get());
		for (;;) {
			const std::wstring_view name{
				info->FileName, info->FileNameLength / sizeof(WCHAR)
			};
			if (name != L"." && name != L"..") {
				FILETIME lastWrite{
					info->LastWriteTime.LowPart,
					static_cast<DWORD>(info->LastWriteTime.HighPart)
				};
				entries.push_back(DirEntry{
				  wideToAnsi(info->FileName, static_cast<int>(name.size())),
				  (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
				  static_cast<int>(GetUnixtimeFromFiletime(lastWrite)) });
			}
			if (info->NextEntryOffset == 0) {
				break;
			}
			info = reinterpret_cast<const FILE_DIRECTORY_INFORMATION*>(
			  reinterpret_cast<const BYTE*>(info) + info->NextEntryOffset);
		}
	}

	return entries;
}

#else

std::vector<DirEntry>
EnumerateDirectory(CSTR /*directory*/)
{
	// FIXME(linux): stub, matching SearchSongsFromPath's non-Windows behavior.
	return {};
}

#endif // _WIN32

std::vector<BMSMETA>
ParseChartsParallel(std::span<const CSTR> paths)
{
	std::vector<BMSMETA> results(paths.size());
	if (paths.empty()) {
		return results;
	}

	const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
	const size_t workerCount =
	  std::min(static_cast<size_t>(hardware), paths.size());

	std::atomic<size_t> nextIndex{ 0 };
	{
		std::vector<std::jthread> workers;
		workers.reserve(workerCount);
		for (size_t w = 0; w < workerCount; ++w) {
			workers.emplace_back([&] {
				for (size_t i = nextIndex.fetch_add(1); i < paths.size();
				     i = nextIndex.fetch_add(1)) {
					ParseBMSMETA(&results[i], paths[i], 0);
				}
			});
		}
	} // std::jthread joins every worker here.

	return results;
}
