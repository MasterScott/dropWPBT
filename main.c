#include <efi.h>
#include <efilib.h>
#include "effy/src/acpi_dump.h"
#include "effy/src/nstdlib.h"
#include "effy/src/xsdt.h"
#include "effy/src/acpi_checksum.h"
#include "effy/src/dirtool.h"
#include "effy/src/chainload.h"

// #define LOAD_WINDOWS

// loads an efi executable if not a DEBUG build;
// otherwise try to read the file and verify if it is an efi executable, then quit
EFI_STATUS load_efi_image(DIRTOOL_FILE* file, EFI_HANDLE ImageHandle)
{
#if defined(_DEBUG)
	Print(L"ChainloadByDevicePath %s ignored in debug build\n", file->Path);
	CHAR8* buf = dirtool_read_file(file);
	Print(L"buf: %c%c\n", buf[0], buf[1]); // should print "MZ"
	return EFI_SUCCESS;
#else
	ClearInputBuf();
	EFI_STATUS ret = ChainloadByDevicePath(file->DevicePath, ImageHandle);
	return ret;
#endif
}

// Application entrypoint (must be set to 'efi_main' for gnu-efi crt0 compatibility)
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable)
{
#if defined(_GNU_EFI)
	InitializeLib(ImageHandle, SystemTable);
#endif

	Print(L"\n%HdropWPBT%N\n");
	Print(L"%Hhttps://github.com/Jamesits/dropWPBT%N\n");
	Print(L"Firmware %s Rev %d\n\n", SystemTable->FirmwareVendor, SystemTable->FirmwareRevision);

	EFI_CONFIGURATION_TABLE* ect = SystemTable->ConfigurationTable;
	EFI_GUID AcpiTableGuid = ACPI_TABLE_GUID;
	EFI_GUID Acpi2TableGuid = ACPI_20_TABLE_GUID;
	EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER* rsdp = NULL;
	EFI_ACPI_SDT_HEADER* Xsdt = NULL;

	UINT64 ret = EFI_SUCCESS;

	// locate RSDP (Root System Description Pointer) 
	for (UINTN SystemTableIndex = 0; SystemTableIndex < SystemTable->NumberOfTableEntries; SystemTableIndex++)
	{
		Print(L"Table #%d/%d: ", SystemTableIndex + 1, SystemTable->NumberOfTableEntries);

		if (!CompareGuid(&SystemTable->ConfigurationTable[SystemTableIndex].VendorGuid, &AcpiTableGuid) && !CompareGuid(
			&SystemTable->ConfigurationTable[SystemTableIndex].VendorGuid, &Acpi2TableGuid))
		{
			Print(L"Not ACPI\n");
			goto next_table;
		}

		if (strncmp8((unsigned char*)"RSD PTR ", (CHAR8*)ect->VendorTable, 8))
		{
			Print(L"Not RSDP\n");
			goto next_table;
		}

		rsdp = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER*)ect->VendorTable;
		Print(L"RSDP Rev %u @0x%x | ", rsdp->Revision, rsdp);

		// check if we have XSDT
		if (rsdp->Revision < EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER_REVISION)
		{
			Print(L"%ENo XSDT\n");
			rsdp = NULL;
			goto next_table;
		}

		// validate XSDT signature
		Xsdt = (EFI_ACPI_SDT_HEADER*)(rsdp->XsdtAddress);
		if (strncmp8((CHAR8*)"XSDT", Xsdt->Signature, 4))
		{
			Print(L"%EInvalid XSDT\n");
			Xsdt = NULL;
			goto next_table;
		}

		// yeah we got XSDT!
		CHAR16 OemStr[20];
		Ascii2UnicodeStr((CHAR8*)(Xsdt->OemId), OemStr, 6);
		UINT32 EntryCount = (Xsdt->Length - sizeof(EFI_ACPI_SDT_HEADER)) / sizeof(UINT64);
		Print(L"%HXSDT OEM ID: %s Tables: %d%N\n", OemStr, EntryCount);
		break;

	next_table:
		ect++;
	}

	if (rsdp && Xsdt)
	{
		// find and destroy WPBT
		XSDT* xsdt1 = (XSDT*)Xsdt;
		UINT32 EntryCount = (Xsdt->Length - sizeof(EFI_ACPI_SDT_HEADER)) / sizeof(UINT64);
		for (UINT32 i = 0; i < EntryCount; ++i)
		{
			EFI_ACPI_DESCRIPTION_HEADER* currentTable = (EFI_ACPI_DESCRIPTION_HEADER*)xsdt1->Entry[i];
			if (currentTable->Signature == 'TBPW')
			{
				Print(L"WPBT table found at position %d/%d\n", i + 1, EntryCount);
				// simply screw up its signature
				currentTable->Signature = 'LSMN';
				currentTable->Checksum -= AcpiChecksum((UINT8*)currentTable, currentTable->Length);
			}
		}
	}
	else if (rsdp == NULL)
	{
		Print(L"%EERROR: RSDP is not found%N\n");
		ret = EFI_UNSUPPORTED;
	}
	else if (Xsdt == NULL)
	{
		Print(L"%EERROR: XSDT is not found%N\n");
		ret = EFI_UNSUPPORTED;
	}
	else
	{
		Print(L"%EError: Something happened%N\n");
		ret = EFI_UNSUPPORTED;
	}

#if defined(_DEBUG)
	Print(L"%EdropWPBT done, press any key to continue.%N\n\n");

	pause();

	// If running in debug mode, use the EFI shut down call to close QEMU
	/*Print(L"%EResetting system%N\n\n");
	SystemTable->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);*/
#else
	// if we are running as an EFI driver, then just quit and let other things load
	Print(L"%EdropWPBT done%N\n\n");
#endif

#if defined(LOAD_WINDOWS)
	// directly load Windows
	Print(L"%HSearching Microsoft bootloader...%N\n");
	CHAR16 MSBootloaderPath1[] = L"EFI\\Microsoft\\Boot\\bootmgfw.efi";
	DIRTOOL_STATE DirToolState;
	DirToolState.initialized = 0;
	// Print(L"dirtool_init\n");
	EFI_STATUS status = dirtool_init(&DirToolState, ImageHandle);
	if (EFI_ERROR(status)) {
		return status;
	}

	// first try current disk
	DIRTOOL_DRIVE* drive;
	drive = dirtool_get_current_drive(&DirToolState, 0);
	dirtool_open_drive(&DirToolState, drive);
	DIRTOOL_FILE* pwd = dirtool_cd_multi(&(drive->RootFile), MSBootloaderPath1);
	if (pwd) load_efi_image(pwd, ImageHandle);
	dirtool_close_drive(&DirToolState, drive);

	if (!pwd) {
		Print(L"%Ebootmgfw.efi not found on current ESP partition, searching all disks%N\n");

#if defined(_DEBUG)
		pause();
#endif

		// then try to search every readable disk for bootmgfw.efi
		DIRTOOL_DRIVE_ITERATOR* iterator = dirtool_drive_iterator_start(&DirToolState);
		while ((drive = dirtool_drive_iterator_next(&DirToolState, iterator)) != NULL)
		{
			DIRTOOL_FILE* pwd = dirtool_open_drive(&DirToolState, drive);
			if (pwd == NULL) continue;
			pwd = dirtool_cd_multi(pwd, MSBootloaderPath1);
			if (pwd) load_efi_image(pwd, ImageHandle);
#if defined(_DEBUG)
			Print(L"before dirtool_close_drive()\n");
			pause();
#endif
			dirtool_close_drive(&DirToolState, drive);
		}
		dirtool_drive_iterator_end(&DirToolState, iterator);
		iterator = NULL;
	}

	dirtool_deinit(&DirToolState);
#endif

	return ret;
}

