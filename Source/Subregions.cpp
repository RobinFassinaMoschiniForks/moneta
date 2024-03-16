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
#include "Memory.hpp"
#include "Interface.hpp"
#include "Processes.hpp"

using namespace std;
using namespace Memory;
using namespace Processes;

Subregion::Subregion(Processes::Process &OwnerProc, const MEMORY_BASIC_INFORMATION* Mbi) : ProcessHandle(OwnerProc.GetHandle()), Basic(Mbi) {
	vector<Processes::Thread*> Threads = OwnerProc.GetThreads();
	vector<void*> Heaps = OwnerProc.GetHeaps();

	for (vector<Processes::Thread*>::const_iterator ThItr = Threads.begin(); ThItr != Threads.end(); ++ThItr) {
		if ((*ThItr)->GetEntryPoint() >= this->Basic->BaseAddress && (*ThItr)->GetEntryPoint() < (static_cast<uint8_t *>(this->Basic->BaseAddress) + this->Basic->RegionSize)) {
			this->Threads.push_back(new Processes::Thread((*ThItr)->GetTid(), OwnerProc));
		}

		if ((*ThItr)->GetStackAddress() >= this->Basic->BaseAddress && (*ThItr)->GetStackAddress() < (static_cast<uint8_t*>(this->Basic->BaseAddress) + this->Basic->RegionSize)) {
			this->Flags |= MEMORY_SUBREGION_FLAG_STACK;
		}

		if ((*ThItr)->GetTebAddress() >= this->Basic->BaseAddress && (*ThItr)->GetTebAddress() < (static_cast<uint8_t*>(this->Basic->BaseAddress) + this->Basic->RegionSize)) {
			this->Flags |= MEMORY_SUBREGION_FLAG_TEB;
		}
	}

	if (OwnerProc.GetImageBase() >= this->Basic->BaseAddress && OwnerProc.GetImageBase() < (static_cast<uint8_t*>(this->Basic->BaseAddress) + this->Basic->RegionSize)) {
		this->Flags |= MEMORY_SUBREGION_FLAG_BASE_IMAGE;
	}

	if (find(Heaps.begin(), Heaps.end(), Mbi->BaseAddress) != Heaps.end()) {
		this->Flags |= MEMORY_SUBREGION_FLAG_HEAP;
	}

	if (Mbi->State == MEM_COMMIT && Mbi->Type != MEM_PRIVATE) {
		this->PrivateSize = this->QueryPrivateSize(); // Querying the working set is one of the greatest performance drains in the tool and should be done sparingly
	}
}

Subregion::~Subregion() {
	if (this->Basic != nullptr) {
		delete Basic;
	}

	for (vector<Processes::Thread*>::const_iterator Itr = this->Threads.begin(); Itr != this->Threads.end(); ++Itr) {
		delete* Itr;
	}
}

const wchar_t* Subregion::ProtectSymbol(uint32_t dwProtect) {
	switch (dwProtect) {
		case PAGE_READONLY: return L"R";
		case PAGE_READWRITE: return L"RW";
		case PAGE_EXECUTE_READ: return L"RX";
		case PAGE_EXECUTE_READWRITE: return L"RWX";
		case PAGE_EXECUTE_WRITECOPY: return L"RWXC";
		case PAGE_EXECUTE: return L"X";
		case PAGE_WRITECOPY: return L"WC";
		case PAGE_NOACCESS: return L"NA";
		case PAGE_WRITECOMBINE: return L"WCB";
		case (PAGE_GUARD | PAGE_READWRITE): // Typically these flags are never combined: page guard is an exception
		case PAGE_GUARD: return L"PG";
		case PAGE_NOCACHE: return L"NC";
		case 0: return L"-";
		default:  return L"?";
	}
}

const wchar_t* Subregion::StateSymbol(uint32_t dwState) {
	switch (dwState) {
		case MEM_COMMIT: return L"Commit";
		case MEM_FREE: return L"Free";
		case MEM_RESERVE: return L"Reserve";
		default: return L"?";
	}
}

const wchar_t* Subregion::AttribDesc(const MEMORY_BASIC_INFORMATION* Mbi) {
	switch (Mbi->State) {
		case MEM_COMMIT: return ProtectSymbol(Mbi->Protect);
		case MEM_FREE: return L"Free";
		case MEM_RESERVE: return L"Reserve";
		default: return L"?";
	}
}

const wchar_t* Subregion::TypeSymbol(uint32_t dwType) {
	switch (dwType) {
		case MEM_IMAGE: return L"IMG";
		case MEM_MAPPED: return L"MAP";
		case MEM_PRIVATE: return L"PRV";
		default: return L"?";
	}
}

uint32_t Subregion::QueryPrivateSize() const {
	static RtlGetVersion_t RtlGetVersion = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
	static RTL_OSVERSIONINFOW osvi = { 0 };
	uint32_t dwPrivateSize = 0;

	if (RtlGetVersion && !osvi.dwBuildNumber) {
		osvi.dwOSVersionInfoSize = sizeof(osvi);
		RtlGetVersion(&osvi);
	}

	if (this->Basic->State == MEM_COMMIT && this->Basic->Protect != PAGE_NOACCESS && this->Basic->Type == MEM_IMAGE) { // Optimize performance by skipping working set scan for non-image memory, as this data is not valuable for private and mapped types.
		MEMORY_WORKING_SET_EX_INFORMATION WorkingSets = { 0 };

		for (uint32_t dwPageOffset = 0; dwPageOffset < this->Basic->RegionSize; dwPageOffset += 0x1000) {
			WorkingSets.VirtualAddress = (static_cast<uint8_t *>(this->Basic->BaseAddress) + dwPageOffset);
			if (K32QueryWorkingSetEx(this->ProcessHandle, &WorkingSets, sizeof(MEMORY_WORKING_SET_EX_INFORMATION))) {
				// Use SharedOriginal after RS3/1709
				// https://windows-internals.com/understanding-a-new-mitigation-module-tampering-protection/
				if (osvi.dwBuildNumber >= 16299) {
					if (!WorkingSets.VirtualAttributes.SharedOriginal) {
						dwPrivateSize += 0x1000;
					}
				} else {
					if (!WorkingSets.VirtualAttributes.Shared) {
						dwPrivateSize += 0x1000;
					}
				}
			}
			else {
				Interface::Log(Interface::VerbosityLevel::Debug, "... failed to query working set at 0x%p\r\n", WorkingSets.VirtualAddress);
			}
		}
	}

	return dwPrivateSize;
}

bool Subregion::PageExecutable(uint32_t dwProtect) {
	return (dwProtect == PAGE_EXECUTE || dwProtect == PAGE_EXECUTE_READ || dwProtect == PAGE_EXECUTE_READWRITE);
}