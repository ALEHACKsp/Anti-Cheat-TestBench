/*
 * ekknod@2021-2022
 *
 * these methods were part of my hobby anti-cheat, and decided to make it public.
 * it's targetted against common kernel drivers.
 * 
 * current methods:
 * - Catch hidden / Unlinked system threads
 * - Catch execution outside of valid module range
 * - Catch KeStackAttachMemory/MmCopyVirtualMemory/ReadProcessMemory
 * - Catch manual MouseClassServiceCallback call
 */

#include <intrin.h>
#include <ntifs.h>



#define TARGET_PROCESS "csgo.exe"


typedef struct _KPRCB* PKPRCB;

__declspec(dllimport) PKPRCB
KeQueryPrcbAddress(
	__in ULONG Number
);

#ifndef CUSTOMTYPES
#define CUSTOMTYPES
typedef ULONG_PTR QWORD;
typedef ULONG DWORD;
typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
#endif


__declspec(dllimport)
PCSTR PsGetProcessImageFileName(QWORD process);


PDRIVER_OBJECT gDriverObject;
PVOID thread_object;
HANDLE thread_handle;
BOOLEAN gExitCalled;

QWORD _KeAcquireSpinLockAtDpcLevel;
QWORD _KeReleaseSpinLockFromDpcLevel;
QWORD _IofCompleteRequest;
QWORD _IoReleaseRemoveLockEx;

BOOL mouse_hook(void);
void mouse_unhook(void);

__declspec(dllimport)
BOOLEAN PsGetProcessExitProcessCalled(PEPROCESS process);

__declspec(dllimport)
PCSTR PsGetProcessImageFileName(QWORD process);

__declspec(dllimport)
QWORD PsGetProcessWow64Process(PEPROCESS process);

__declspec(dllimport)
QWORD PsGetProcessPeb(PEPROCESS process);

QWORD GetModuleHandle(PWCH module_name, QWORD *SizeOfImage);

QWORD GetProcessByName(const char* process_name)
{
	QWORD process;
	QWORD entry;

	DWORD gActiveProcessLink = *(DWORD*)((char*)PsGetProcessId + 3) + 8;
	process = (QWORD)PsInitialSystemProcess;

	entry = process;
	do {
		if (PsGetProcessExitProcessCalled((PEPROCESS)entry))
			goto L0;

		if (PsGetProcessImageFileName(entry) && strcmp(PsGetProcessImageFileName(entry), process_name) == 0) {
			return entry;
		}
	L0:
		entry = *(QWORD*)(entry + gActiveProcessLink) - gActiveProcessLink;
	} while (entry != process);

	return 0;
}

BOOL IsThreadFoundEPROCESS(QWORD process, QWORD thread)
{
	BOOL contains = 0;

	QWORD address = (QWORD)PsGetThreadExitStatus;
	address += 0xA;
	DWORD RunDownProtectOffset = *(DWORD*)(address + 3);
	ULONG ThreadListEntryOffset = RunDownProtectOffset - 0x10;

	PLIST_ENTRY ThreadListEntry = (PLIST_ENTRY)((QWORD)process + *(UINT32*)((char*)PsGetProcessImageFileName + 3) + 0x38);
	PLIST_ENTRY list = ThreadListEntry;

	while ((list = list->Flink) != ThreadListEntry) {


		QWORD ethread_entry = (QWORD)((char*)list - ThreadListEntryOffset);
		if (ethread_entry == thread) {
			contains = 1;
			break;
		}
	}

	return contains;
}

void NtSleep(DWORD milliseconds)
{
	QWORD ms = milliseconds;
	ms = (ms * 1000) * 10;
	ms = ms * -1;
#ifdef _KERNEL_MODE
	KeDelayExecutionThread(KernelMode, 0, (PLARGE_INTEGER)&ms);
#else
	NtDelayExecution(0, (PLARGE_INTEGER)&ms);
#endif
}

__declspec(dllimport)
NTSTATUS
PsGetContextThread(
      __in PETHREAD Thread,
      __inout PCONTEXT ThreadContext,
      __in KPROCESSOR_MODE Mode
  );

BOOL IsInValidRange(QWORD address);


PKPRCB KeGetCurrentPrcb (VOID)
{

	
    return (PKPRCB) __readgsqword (FIELD_OFFSET (KPCR, CurrentPrcb));
}

void ThreadDetection(QWORD target_game)
{
	/*
	 * I'm not focusing to make clean code, this is just anti-cheat test bench.
	 * 
	 * I have written this in just couple minutes,
	 * that's why it's repeating a lot. who cares :D
	 * 
	 * logic is easy to understand at least.
	 * 
	 * 
	 * Modern Anti-Cheats what doesn't go through KPRCB properly:
	 * ESPORTAL/Vanguard/ESEA/EAC(?)/BE(?)
	 *
	 */

	QWORD current_thread, next_thread;

	for (int i = 0; i < KeNumberProcessors; i++) {
		PKPRCB prcb = KeQueryPrcbAddress(i);


		if (prcb == 0)
			continue;


		current_thread = *(QWORD*)((QWORD)prcb + 0x8);
		if (current_thread != 0) {

			if (current_thread == (QWORD)PsGetCurrentThread())
				goto skip_current;

			if (PsGetThreadExitStatus((PETHREAD)current_thread) != STATUS_PENDING)
				goto skip_current;


			CONTEXT ctx = { 0 };
			ctx.ContextFlags = CONTEXT_ALL;
			

			QWORD cid = (QWORD)PsGetThreadId((PETHREAD)current_thread);
			QWORD host_process = *(QWORD*)(current_thread + 0x220);

			BOOL hidden = 0, invalid_range=0;

			
			
			// if (KeGetCurrentPrcb() != prcb) ; I don't think this is anymore required after SpecialApcDisable check
			{
				// SpecialApcDisable 0x1e6
				if (*(SHORT*)(current_thread + 0x1e6) == 0 && NT_SUCCESS(PsGetContextThread((PETHREAD)current_thread, &ctx, KernelMode))) {
					if (!IsInValidRange(ctx.Rip)) {

						DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[%s] Thread is outside of valid module [%ld, %llx] RIP[%llx]\n",
							PsGetProcessImageFileName(host_process),
							cid,
							current_thread,
							ctx.Rip
						);
						invalid_range = 1;
					}
				}
				
			}
			

			if (!IsThreadFoundEPROCESS(host_process, current_thread))
			{
				hidden = 1;
				DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Hidden thread found [%s %d], %llx, %d]\n",
					PsGetProcessImageFileName(host_process),

					PsGetProcessId((PEPROCESS)host_process),

					current_thread,
					(DWORD)cid
				);
			}


			// if (thread->ApcState.Process == target_game_process) 
			if (target_game && target_game != host_process && *(QWORD*)(current_thread + 0x98 + 0x20) == target_game) {


				// small filter before proper validating
				BOOL temporary_whitelist = 0;
				if (host_process == (QWORD)PsGetCurrentProcess() && !hidden && !invalid_range)
				{
					temporary_whitelist = 1;
				}

				char* target_str;
				if (hidden)
					target_str = "[%s] Hidden Thread (%llx, %ld) is attached to %s\n";
				else
					target_str = "[%s] Thread (%llx, %ld) is attached to %s\n";


				if (!temporary_whitelist)
				DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, target_str,
					PsGetProcessImageFileName(host_process),
					current_thread,
					(DWORD)cid,
					PsGetProcessImageFileName(*(QWORD*)(current_thread + 0x98 + 0x20))
				);

			}

			

		}
	skip_current:

		next_thread = *(QWORD*)((QWORD)prcb + 0x10);


		if (next_thread) {

			if (next_thread == (QWORD)PsGetCurrentThread())
				continue;

			if (PsGetThreadExitStatus((PETHREAD)next_thread) != STATUS_PENDING)
				continue;



			QWORD cid = (QWORD)PsGetThreadId((PETHREAD)next_thread);
			QWORD host_process = *(QWORD*)(next_thread + 0x220);


			BOOL hidden = 0, invalid_range=0;

			CONTEXT ctx = { 0 };
			ctx.ContextFlags = CONTEXT_ALL;

			// if (KeGetCurrentPrcb() != prcb) ; I don't think this is anymore required after SpecialApcDisable check
			{
				// SpecialApcDisable 0x1e6
				if (*(SHORT*)(next_thread + 0x1e6) == 0 && NT_SUCCESS(PsGetContextThread((PETHREAD)next_thread, &ctx, KernelMode))) {
					
					if (!IsInValidRange(ctx.Rip)) {

						DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[%s] Thread is outside of valid module [%ld, %llx] RIP[%llx]\n",
							PsGetProcessImageFileName(host_process),
							cid,
							next_thread,
							ctx.Rip
						);
						invalid_range=1;
					}
				}
				
			}


			if (!IsThreadFoundEPROCESS(host_process, next_thread))
			{
				hidden = 1;

				DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Hidden thread found [%s %d], %llx, %d]\n",
					PsGetProcessImageFileName(host_process),

					PsGetProcessId((PEPROCESS)host_process),

					next_thread,
					(DWORD)cid
				);
			}

			// if (thread->ApcState.Process == target_game_process) 
			if (target_game && target_game != host_process && *(QWORD*)(next_thread + 0x98 + 0x20) == target_game) {
				// small filter before proper validating
				BOOL temporary_whitelist = 0;
				if (host_process == (QWORD)PsGetCurrentProcess() && !hidden && !invalid_range)
				{
					temporary_whitelist = 1;
				}


				char* target_str;
				if (hidden)
					target_str = "[%s] Hidden Thread (%llx, %ld) is attached to %s\n";
				else
					target_str = "[%s] Thread (%llx, %ld) is attached to %s\n";


				if (!temporary_whitelist)
					DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[%s] Thread (%llx, %ld) is attached to %s\n",
						PsGetProcessImageFileName(host_process),
						next_thread,
						(DWORD)cid,
						PsGetProcessImageFileName(*(QWORD*)(next_thread + 0x98 + 0x20))
					);

			}

		}


	}
}

VOID
DriverUnload(
	_In_ struct _DRIVER_OBJECT* DriverObject
)
{

	(DriverObject);
	gExitCalled = 1;

	mouse_unhook();

	if (thread_object) {
		KeWaitForSingleObject(
			(PVOID)thread_object,
			Executive,
			KernelMode,
			FALSE,
			0
		);

		ObDereferenceObject(thread_object);

		ZwClose(thread_handle);
	}


	// Lets wait second, everything should be unloaded now.
	NtSleep(1000);

	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[+] Anti-Cheat.sys is closed\n");
}


__declspec(dllimport)
BOOLEAN PsGetProcessExitProcessCalled(PEPROCESS process);

NTSTATUS system_thread(void)
{
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[+] Anti-Cheat.sys is launched\n");


	QWORD target_game = 0;

	while (gExitCalled == 0) {

		NtSleep(1);

		

		if (target_game == 0 || PsGetProcessExitProcessCalled((PEPROCESS)target_game)) {
			target_game    = GetProcessByName(TARGET_PROCESS);

			if (target_game == 0)
				goto skip_address;
		}




	skip_address:


		/*
		 * Detect system hidden threads
		 * Detect virtual memory access for our target game
		 */
		ThreadDetection(target_game);

	}

	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[+] Anti-Cheat.sys thread is closed\n");

	return 0l;
}

typedef struct {
	QWORD base,size;
} IMAGE_INFO_TABLE ;
IMAGE_INFO_TABLE vmusbmouse;

NTSTATUS DriverEntry(
	_In_ PDRIVER_OBJECT  DriverObject,
	_In_ PUNICODE_STRING RegistryPath
)
{

	(DriverObject);
	(RegistryPath);

	gDriverObject = DriverObject;
	DriverObject->DriverUnload = DriverUnload;

	vmusbmouse.base = GetModuleHandle(L"vmusbmouse.sys", &vmusbmouse.size);

	mouse_hook();

	CLIENT_ID thread_id;
	PsCreateSystemThread(&thread_handle, STANDARD_RIGHTS_ALL, NULL, NULL, &thread_id, (PKSTART_ROUTINE)system_thread, (PVOID)0);
	ObReferenceObjectByHandle(
		thread_handle,
		THREAD_ALL_ACCESS,
		NULL,
		KernelMode,
		(PVOID*)&thread_object,
		NULL
	);

	return STATUS_SUCCESS;
}

typedef struct _KLDR_DATA_TABLE_ENTRY {
        LIST_ENTRY InLoadOrderLinks;
        VOID* ExceptionTable;
        UINT32 ExceptionTableSize;
        VOID* GpValue;
        VOID* NonPagedDebugInfo;
        VOID* ImageBase;
        VOID* EntryPoint;
        UINT32 SizeOfImage;
        UNICODE_STRING FullImageName;
        UNICODE_STRING BaseImageName;
} LDR_DATA_TABLE_ENTRY, *PLDR_DATA_TABLE_ENTRY;


BOOL IsInValidRange(QWORD address)
{
	PLDR_DATA_TABLE_ENTRY ldr = (PLDR_DATA_TABLE_ENTRY)gDriverObject->DriverSection;
	for (PLIST_ENTRY pListEntry = ldr->InLoadOrderLinks.Flink; pListEntry != &ldr->InLoadOrderLinks; pListEntry = pListEntry->Flink)
	{
		
		PLDR_DATA_TABLE_ENTRY pEntry = CONTAINING_RECORD(pListEntry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

		if (pEntry->ImageBase == 0)
			continue;

		/* 0x1000 discardable section. this should be manually verified from image nt (0x3C + 0x50) * 
		but because this is non serious AC we assume all modules has it */
		if (address >= (QWORD)pEntry->ImageBase && address <= (QWORD)((QWORD)pEntry->ImageBase + pEntry->SizeOfImage + 0x1000 ))
			return 1;
		
	}

	return 0;
}

QWORD GetModuleHandle(PWCH module_name, QWORD *SizeOfImage)
{
	PLDR_DATA_TABLE_ENTRY ldr = (PLDR_DATA_TABLE_ENTRY)gDriverObject->DriverSection;
	for (PLIST_ENTRY pListEntry = ldr->InLoadOrderLinks.Flink; pListEntry != &ldr->InLoadOrderLinks; pListEntry = pListEntry->Flink)
	{
		
		PLDR_DATA_TABLE_ENTRY pEntry = CONTAINING_RECORD(pListEntry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);

		if (pEntry->BaseImageName.Buffer && wcscmp(pEntry->BaseImageName.Buffer, module_name) == 0) {
			*SizeOfImage = 0;
			*SizeOfImage = pEntry->SizeOfImage;
			return (QWORD)pEntry->ImageBase;
		}
		
	}
	
	return 0;
}

#pragma warning(disable : 4201)
typedef struct _MOUSE_INPUT_DATA {
	USHORT UnitId;
	USHORT Flags;
	union {
	ULONG Buttons;
	struct {
	USHORT ButtonFlags;
	USHORT ButtonData;
	};
	};
	ULONG  RawButtons;
	LONG   LastX;
	LONG   LastY;
	ULONG  ExtraInformation;
} MOUSE_INPUT_DATA, *PMOUSE_INPUT_DATA;

typedef VOID
(*MouseClassServiceCallbackFn)(
	PDEVICE_OBJECT DeviceObject,
	PMOUSE_INPUT_DATA InputDataStart,
	PMOUSE_INPUT_DATA InputDataEnd,
	PULONG InputDataConsumed
);

typedef struct _MOUSE_OBJECT
{
	PDEVICE_OBJECT              mouse_device;
	MouseClassServiceCallbackFn service_callback;
	BOOL                        hook;
	QWORD                       hid;
	QWORD                       hid_length;
} MOUSE_OBJECT, * PMOUSE_OBJECT;

MOUSE_OBJECT gMouseObject;


// https://github.com/btbd/umap/blob/master/mapper/util.c#L117
BOOLEAN MemCopyWP(PVOID dest, PVOID src, ULONG length) {
	PMDL mdl = IoAllocateMdl(dest, length, FALSE, FALSE, NULL);
	if (!mdl) {
		return FALSE;
	}

	MmProbeAndLockPages(mdl, KernelMode, IoModifyAccess);

	PVOID mapped = MmMapLockedPagesSpecifyCache(mdl, KernelMode, MmNonCached, NULL, 0, HighPagePriority);
	if (!mapped) {
		MmUnlockPages(mdl);
		IoFreeMdl(mdl);
		return FALSE;
	}

	memcpy(mapped, src, length);

	MmUnmapLockedPages(mapped, mdl);
	MmUnlockPages(mdl);
	IoFreeMdl(mdl);
	return TRUE;
}

#define JMP_SIZE 14

// https://github.com/btbd/umap/
VOID *TrampolineHook(VOID *dest, VOID *src, UINT8 original[JMP_SIZE]) {
	if (original) {
		MemCopyWP(original, src, JMP_SIZE);
	}

	unsigned char bytes[] = "\xFF\x25\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";
	*(QWORD*)(bytes + 6) = (QWORD)dest;

	MemCopyWP(src, bytes, JMP_SIZE);

	return src;
}

VOID TrampolineUnHook(VOID *src, UINT8 original[JMP_SIZE]) {
	MemCopyWP(src, original, JMP_SIZE);
}



QWORD MouseClassServiceCallback(
	PDEVICE_OBJECT DeviceObject,
	PMOUSE_INPUT_DATA InputDataStart,
	PMOUSE_INPUT_DATA InputDataEnd,
	PULONG InputDataConsumed
);


QWORD MouseClassServiceCallbackHook(
	PDEVICE_OBJECT DeviceObject,
	PMOUSE_INPUT_DATA InputDataStart,
	PMOUSE_INPUT_DATA InputDataEnd,
	PULONG InputDataConsumed
)
{
	(DeviceObject);
	(InputDataStart);
	(InputDataEnd);
	(InputDataConsumed);

	QWORD address = (QWORD)_ReturnAddress();

	
	if (address < (QWORD)gMouseObject.hid || address > (QWORD)((QWORD)gMouseObject.hid + gMouseObject.hid_length))
	{
		// extra check for vmware virtual machine
		if (address < (QWORD)vmusbmouse.base || address > (QWORD)((QWORD)vmusbmouse.base + vmusbmouse.size))
			DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Mouse manipulation was detected (%llx)\n", _ReturnAddress());
	}

	// C/C++ -> All Options -> Control Overflow Guard : OFF, otherwise compiler will create CALL instruction and it will BSOD.
	// Sadly inline assembly is not supported by x64 C/C++.
	return ((QWORD(*)(PDEVICE_OBJECT, PMOUSE_INPUT_DATA, PMOUSE_INPUT_DATA, PULONG))((QWORD)MouseClassServiceCallback + 5))(
		DeviceObject,
		InputDataStart,
		InputDataEnd,
		InputDataConsumed
		);

	
}


unsigned char OriginalMouseClassService[JMP_SIZE];

NTSYSCALLAPI
NTSTATUS
ObReferenceObjectByName(
      __in PUNICODE_STRING ObjectName,
      __in ULONG Attributes,
      __in_opt PACCESS_STATE AccessState,
      __in_opt ACCESS_MASK DesiredAccess,
      __in POBJECT_TYPE ObjectType,
      __in KPROCESSOR_MODE AccessMode,
      __inout_opt PVOID ParseContext,
      __out PVOID *Object
  );

NTSYSCALLAPI
POBJECT_TYPE* IoDriverObjectType;

BOOL mouse_hook(void)
{
	
	_KeAcquireSpinLockAtDpcLevel = (QWORD)KeAcquireSpinLockAtDpcLevel;
	_KeReleaseSpinLockFromDpcLevel = (QWORD)KeReleaseSpinLockFromDpcLevel;
	_IofCompleteRequest = (QWORD)IofCompleteRequest;
	_IoReleaseRemoveLockEx = (QWORD)IoReleaseRemoveLockEx;

	// https://github.com/nbqofficial/norsefire
	if (gMouseObject.hook == 0) {

		UNICODE_STRING class_string;
		RtlInitUnicodeString(&class_string, L"\\Driver\\MouClass");
	

		PDRIVER_OBJECT class_driver_object = NULL;
		NTSTATUS status = ObReferenceObjectByName(&class_string, OBJ_CASE_INSENSITIVE, NULL, 0, *IoDriverObjectType, KernelMode, NULL, (PVOID*)&class_driver_object);
		if (!NT_SUCCESS(status)) {
			return 0;
		}

		UNICODE_STRING hid_string;
		RtlInitUnicodeString(&hid_string, L"\\Driver\\MouHID");
	

		PDRIVER_OBJECT hid_driver_object = NULL;
	
		status = ObReferenceObjectByName(&hid_string, OBJ_CASE_INSENSITIVE, NULL, 0, *IoDriverObjectType, KernelMode, NULL, (PVOID*)&hid_driver_object);
		if (!NT_SUCCESS(status))
		{
			if (class_driver_object) {
				ObfDereferenceObject(class_driver_object);
			}
			return 0;
		}

		gMouseObject.hid = (QWORD)hid_driver_object->DriverStart;
		gMouseObject.hid_length = (QWORD)hid_driver_object->DriverSize;

		PVOID class_driver_base = NULL;


		PDEVICE_OBJECT hid_device_object = hid_driver_object->DeviceObject;
		while (hid_device_object && !gMouseObject.service_callback)
		{
			PDEVICE_OBJECT class_device_object = class_driver_object->DeviceObject;
			while (class_device_object && !gMouseObject.service_callback)
			{
				if (!class_device_object->NextDevice && !gMouseObject.mouse_device)
				{
					gMouseObject.mouse_device = class_device_object;
				}

				PULONG_PTR device_extension = (PULONG_PTR)hid_device_object->DeviceExtension;
				ULONG_PTR device_ext_size = ((ULONG_PTR)hid_device_object->DeviceObjectExtension - (ULONG_PTR)hid_device_object->DeviceExtension) / 4;
				class_driver_base = class_driver_object->DriverStart;
				for (ULONG_PTR i = 0; i < device_ext_size; i++)
				{
					if (device_extension[i] == (ULONG_PTR)class_device_object && device_extension[i + 1] > (ULONG_PTR)class_driver_object)
					{
						gMouseObject.service_callback = (MouseClassServiceCallbackFn)(device_extension[i + 1]);
					
						break;
					}
				}
				class_device_object = class_device_object->NextDevice;
			}
			hid_device_object = hid_device_object->AttachedDevice;
		}
	
		if (!gMouseObject.mouse_device)
		{
			PDEVICE_OBJECT target_device_object = class_driver_object->DeviceObject;
			while (target_device_object)
			{
				if (!target_device_object->NextDevice)
				{
					gMouseObject.mouse_device = target_device_object;
					break;
				}
				target_device_object = target_device_object->NextDevice;
			}
		}

		ObfDereferenceObject(class_driver_object);
		ObfDereferenceObject(hid_driver_object);

		if (gMouseObject.mouse_device && gMouseObject.service_callback) {	

			TrampolineHook((void *)MouseClassServiceCallback, (void *)gMouseObject.service_callback,  OriginalMouseClassService);

			gMouseObject.hook = 1;

			return 1;

		}
	} else {
		return 1;
	}

	return 0;

	
}

void mouse_unhook(void)
{
	if (gMouseObject.hook) {
		TrampolineUnHook((void *)gMouseObject.service_callback, OriginalMouseClassService);
		gMouseObject.hook = 0;
	}
}

