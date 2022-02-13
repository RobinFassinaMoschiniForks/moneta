/*
____________________________________________________________________________________________________
| _______  _____  __   _ _______ _______ _______                                                   |
| |  |  | |     | | \  | |______    |    |_____|                                                   |
| |  |  | |_____| |  \_| |______    |    |     |                                                   |
|__________________________________________________________________________________________________|
| Moneta ~ Usermode memory scanner & malware hunter                                                |
|--------------------------------------------------------------------------------------------------|
| https://www.forrest-orr.net/post/masking-malicious-memory-artifacts-part-ii-insights-from-moneta |
|--------------------------------------------------------------------------------------------------|
| Author: Forrest Orr - 2020                                                                       |
|--------------------------------------------------------------------------------------------------|
| Contact: forrest.orr@protonmail.com                                                              |
|--------------------------------------------------------------------------------------------------|
| Licensed under GNU GPLv3                                                                         |
|__________________________________________________________________________________________________|
| ## Features                                                                                      |
|                                                                                                  |
| ~ Query the memory attributes of any accessible process(es).                                     |
| ~ Identify private, mapped and image memory.                                                     |
| ~ Correlate regions of memory to their underlying file on disks.                                 |
| ~ Identify PE headers and sections corresponding to image memory.                                |
| ~ Identify modified regions of mapped image memory.                                              |
| ~ Identify abnormal memory attributes indicative of malware.                                     |
| ~ Create memory dumps of user-specified memory ranges                                            |
| ~ Calculate memory permission/type statistics                                                    |
|__________________________________________________________________________________________________|

*/

#include "StdAfx.h"
#include "FileIo.hpp"
#include "PeFile.hpp"
#include "Processes.hpp"
#include "Memory.hpp"
#include "Interface.hpp"
#include "MemDump.hpp"
#include "Scanner.hpp"
#include "Privileges.h"
#include "Resources.h"
#include "Statistics.hpp"
#include "Ioc.hpp"

using namespace std;
using namespace Memory;
using namespace Processes;

enum class SelectedProcess_t {
	InvalidPid = 0,
	SpecificPid,
	AllPids,
	SelfPid
};

int32_t wmain(int32_t nArgc, const wchar_t* pArgv[]) {
	vector<wstring> Args(&pArgv[0], &pArgv[0 + nArgc]);
	Interface::Initialize(Args);
	SelectedProcess_t ProcType = SelectedProcess_t::InvalidPid;
	ScannerContext::MemorySelection_t Mst = ScannerContext::MemorySelection_t::Invalid;
	uint32_t dwSelectedPid = 0, dwRegionSize = 0;
	uint8_t* pAddress = nullptr;
	bool bSuppressBanner = false;
	uint64_t qwOptFlags = 0, qwFilterFlags = 0;

	for (vector<wstring>::const_iterator i = Args.begin(); i != Args.end(); ++i) {
		wstring Arg = *i;
		transform(Arg.begin(), Arg.end(), Arg.begin(), ::tolower);

		if (Arg == L"-p") {
			if (*(i + 1) == L"*") {
				ProcType = SelectedProcess_t::AllPids;
			}
			else {
				ProcType = SelectedProcess_t::SpecificPid;
				dwSelectedPid = _wtoi((*(i + 1)).c_str());
			}
		}
		else if (Arg == L"-m") {
			if (*(i + 1) == L"region") {
				Mst = ScannerContext::MemorySelection_t::Block;
			}
			else if (*(i + 1) == L"*") {
				Mst = ScannerContext::MemorySelection_t::All;
			}
			else if (*(i + 1) == L"ioc") {
				Mst = ScannerContext::MemorySelection_t::Ioc;
			}
			else if (*(i + 1) == L"referenced") {
				Mst = ScannerContext::MemorySelection_t::Referenced;
			}
		}
		else if (Arg == L"--address") {
			pAddress = reinterpret_cast<uint8_t*>(wcstoull((*(i + 1)).c_str(), NULL, 0));
		}
		else if (Arg == L"-d") {
			qwOptFlags |= PROCESS_ENUM_FLAG_MEMDUMP;
		}
		else if (Arg == L"--region-size") {
			dwRegionSize = _wtoi((*(i + 1)).c_str());
		}
		else if (Arg == L"--option") {
			for (vector<wstring>::const_iterator OptZtr = i; OptZtr != Args.end(); ++OptZtr) {
				wstring OptArg = *OptZtr;
				transform(OptArg.begin(), OptArg.end(), OptArg.begin(), ::tolower);

				if (OptArg == L"from-base") {
					qwOptFlags |= PROCESS_ENUM_FLAG_FROM_BASE;
				}
				else if (OptArg == L"statistics") {
					qwOptFlags |= PROCESS_ENUM_FLAG_STATISTICS;
				}
				else if (OptArg == L"suppress-banner") {
					bSuppressBanner = true;
				}
			}
		}
		else if (Arg == L"--filter") {
			for (vector<wstring>::const_iterator FilterItr = i; FilterItr != Args.end(); ++FilterItr) {
				wstring FilterArg = *FilterItr;
				transform(FilterArg.begin(), FilterArg.end(), FilterArg.begin(), ::tolower);

				if (FilterArg == L"*") {
					qwFilterFlags = -1;
					break;
				}
				else if (FilterArg == L"unsigned-modules") {
					qwFilterFlags |= FILTER_FLAG_UNSIGNED_MODULES;
				}
				else if (FilterArg == L"metadata-modules") {
					qwFilterFlags |= FILTER_FLAG_METADATA_MODULES;
				}
				else if (FilterArg == L"clr-prvx") {
					qwFilterFlags |= FILTER_FLAG_CLR_PRVX;
				}
				else if (FilterArg == L"clr-heap") {
					qwFilterFlags |= FILTER_FLAG_CLR_HEAP;
				}
				else if (FilterArg == L"wow64-init") {
					qwFilterFlags |= FILTER_FLAG_WOW64_INIT;
				}
			}
		}
	}

	if (!bSuppressBanner) {
		Interface::Log(Interface::VerbosityLevel::Surface,
			"   _____                        __          \r\n"
			"  /     \\   ____   ____   _____/  |______   \r\n"
			" /  \\ /  \\ /  _ \\ /    \\_/ __ \\   __\\__  \\  \r\n"
			"/    Y    (  <_> )   |  \\  ___/|  |  / __ \\_\r\n"
			"\\____|__  /\\____/|___|  /\\___  >__| (____  /\r\n"
			"        \\/            \\/     \\/          \\/ \r\n"
			"\r\n"
			"Moneta v1.1 | Forrest Orr | 2022\r\n\r\n"
		);
	}

	SYSTEM_INFO SystemInfo = { 0 };
	static IsWow64Process_t IsWow64Process = reinterpret_cast<IsWow64Process_t>(GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "IsWow64Process"));
	
	if (IsWow64Process != nullptr) {
		BOOL bSelfWow64 = FALSE;

		if (IsWow64Process(GetCurrentProcess(), static_cast<PBOOL>(&bSelfWow64))) {
			GetNativeSystemInfo(&SystemInfo); // Native version of this call works on both Wow64 and x64 as opposed to just x64 for GetSystemInfo. Works on XP+

			if (SystemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 && bSelfWow64) {
				Interface::Log(Interface::VerbosityLevel::Surface, "... Moneta 32-bit should not be used on a 64-bit OS. Use the x64 version of this tool.\r\n");
				return 0;
			}
		}
	}

	if (nArgc < 5) {
		HMODULE	hSelfModule = GetModuleHandleA(nullptr);
		HRSRC hResourceInfo;
		HGLOBAL hResourceData;
		char* pRsrcData = nullptr;
		uint32_t dwRsrcSize;

		if ((hResourceInfo = FindResourceA(hSelfModule, IDR_USAGE_TEXT_NAME, RT_RCDATA))) {
			if ((hResourceData = LoadResource(hSelfModule, hResourceInfo))) {
				dwRsrcSize = SizeofResource(hSelfModule, hResourceInfo);
				pRsrcData = (char*)LockResource(hResourceData);
				unique_ptr<uint8_t[]> RsrcBuf = make_unique<uint8_t[]>(dwRsrcSize + 1); // Otherwise the resource text may bleed in to the rest of the .rsrc section
				memcpy(RsrcBuf.get(), pRsrcData, dwRsrcSize);
				Interface::Log(Interface::VerbosityLevel::Surface, "%s\r\n", RsrcBuf.get());
			}
		}
	}
	else {
		// Validate user input

		if (ProcType == SelectedProcess_t::InvalidPid) {
			Interface::Log(Interface::VerbosityLevel::Surface, "... invalid target process type selected\r\n");
			return 0;
		}

		if (dwSelectedPid == GetCurrentProcessId()) {
			Interface::Log(Interface::VerbosityLevel::Surface, "... this scanner cannot target itself\r\n");
			return 0;
		}

		if (Mst == ScannerContext::MemorySelection_t::Invalid) {
			Interface::Log(Interface::VerbosityLevel::Surface, "... invalid memory selection type\r\n");
			return 0;
		}

		if ((Mst == ScannerContext::MemorySelection_t::Referenced || Mst == ScannerContext::MemorySelection_t::Block) && pAddress == nullptr) {
			Interface::Log(Interface::VerbosityLevel::Surface, "... address must be specified for the provided memory selection type.\r\n");
			return 0;
		}

		// Initialization

		if (GrantSelfSeDebug()) {
			Interface::Log(Interface::VerbosityLevel::Debug, "... successfully granted SeDebug privilege to self\r\n");
		}
		else {
			Interface::Log(Interface::VerbosityLevel::Surface, "... failed to grant SeDebug privilege to self. Certain processes will be inaccessible.\r\n");
		}

		if ((qwOptFlags & PROCESS_ENUM_FLAG_MEMDUMP)) {
			MemDump::Initialize();
		}

		// Analyze processes and generate memory maps/suspicions

		ScannerContext ScannerCtx(qwOptFlags, Mst, pAddress, dwRegionSize, qwFilterFlags);
		uint64_t qwStartTick = GetTickCount64();

		if (ProcType == SelectedProcess_t::SelfPid || ProcType == SelectedProcess_t::SpecificPid) {
			try {
				Process TargetProc(dwSelectedPid);
				vector<Ioc*> SelectedIocs;
				vector<Subregion*> SelectedSbrs;

				TargetProc.Enumerate(ScannerCtx, &SelectedIocs, &SelectedSbrs);

				if ((qwOptFlags & PROCESS_ENUM_FLAG_STATISTICS)) {
					PermissionRecord PermissionRecords(SelectedSbrs);
					IocRecord IocRecords(&SelectedIocs);
					Interface::SetVerbosity(Interface::VerbosityLevel::Surface); // Override the verbosity level now that the scan is over to ensure statistics and scan time are displayed (if applicable)
					PermissionRecords.ShowRecords();
					IocRecords.ShowRecords();
				}
			}
			catch (int32_t nError) {
				Interface::Log(Interface::VerbosityLevel::Surface, "... failed to map address space of %d (error %d)\r\n", dwSelectedPid, nError);
			}
		}
		else {
			PROCESSENTRY32W ProcEntry = { 0 };
			HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

			unique_ptr<PermissionRecord> PermissionRecords;
			unique_ptr<IocRecord> IocRecords;

			if (hSnapshot != nullptr) {
				ProcEntry.dwSize = sizeof(PROCESSENTRY32W);

				if (Process32FirstW(hSnapshot, &ProcEntry)) {
					do {
						if (ProcEntry.th32ProcessID != GetCurrentProcessId()) {
							try {
								Process TargetProc(ProcEntry.th32ProcessID);
								vector<Ioc*> SelectedIocs;
								vector<Subregion*> SelectedSbrs;

								TargetProc.Enumerate(ScannerCtx, &SelectedIocs, &SelectedSbrs);

								if ((qwOptFlags & PROCESS_ENUM_FLAG_STATISTICS)) {
									if (!PermissionRecords) {
										PermissionRecords = make_unique<PermissionRecord>(SelectedSbrs);
									}
									else {
										PermissionRecords->UpdateMap(SelectedSbrs);
									}

									if (!IocRecords) {
										IocRecords = make_unique<IocRecord>(&SelectedIocs);
									}
									else {
										IocRecords->UpdateMap(&SelectedIocs);
									}
								}
							}
							catch (int32_t nError) {
								Interface::Log(Interface::VerbosityLevel::Debug, "... failed to map address space of %d:%ws (error %d)\r\n", ProcEntry.th32ProcessID, ProcEntry.szExeFile, nError);
								continue;
							}
						}
					} while (Process32NextW(hSnapshot, &ProcEntry));
				}

				CloseHandle(hSnapshot);
			}
			else
			{
				Interface::Log(Interface::VerbosityLevel::Surface, "... failed to create process list snapshot (error %d)\r\n", GetLastError());
			}

			Interface::SetVerbosity(Interface::VerbosityLevel::Surface); // Override the verbosity level now that the scan is over to ensure statistics and scan time are displayed (if applicable)

			if (PermissionRecords) {
				PermissionRecords->ShowRecords();
			}

			if (IocRecords) {
				IocRecords->ShowRecords();
			}
		}

		float fElapsedTime = GetTickCount64() - qwStartTick;
		Interface::Log(Interface::VerbosityLevel::Surface, "\r\n... scan completed (%f second duration)\r\n", fElapsedTime / 1000.0);
		return 1;
	}
}