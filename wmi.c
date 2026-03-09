
/* 
 * NTop - an htop clone for Windows
 * 
 * This program is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <windows.h>
#include <wbemidl.h>
#include <strsafe.h>
#include <tchar.h>
#include <malloc.h>
#include "ntop.h"
#include "util.h"
#include "wmi.h"

static WMI_CACHE_ENTRY *CommandLineCache = NULL;
static DWORD CommandLineCacheCapacity = 0;
static DWORD CommandLineCacheCount = 0;

static WMI_SERVICE_CACHE_ENTRY *ServiceCache = NULL;
static DWORD ServiceCacheCapacity = 0;
static DWORD ServiceCacheCount = 0;

static CRITICAL_SECTION WmiCacheLock;
static BOOL WmiInitialized = FALSE;
static BOOL WmiThreadRunning = FALSE;
static HANDLE WmiThreadHandle = NULL;
static HANDLE WmiStopEvent = NULL;

static DWORD WINAPI WmiCacheThreadProc(LPVOID lpParam);
static void RefreshCommandLineCache(void);
static void RefreshCommandLineCacheInternal(IWbemServices *WmiServices);
static void RefreshServiceCacheInternal(IWbemServices *WmiServices);
static void ReplaceChar(TCHAR* str, TCHAR find, TCHAR replace);
static BOOL EnsureCommandLineCacheCapacity(DWORD required);
static BOOL EnsureServiceCacheCapacity(DWORD required);
static void FreeCommandLineCacheEntry(DWORD index);
static void FreeServiceCacheEntry(DWORD index);
static void ClearCommandLineCache(void);
static void ClearServiceCache(void);

static BOOL EnsureCommandLineCacheCapacity(DWORD required)
{
	if (CommandLineCacheCapacity >= required) {
		return TRUE;
	}

	DWORD newCapacity = CommandLineCacheCapacity == 0 ? WMI_INITIAL_PROCESS_CACHE_CAPACITY : CommandLineCacheCapacity;
	while (newCapacity < required) {
		newCapacity *= WMI_CACHE_GROWTH_FACTOR;
	}

	WMI_CACHE_ENTRY *newCache = (WMI_CACHE_ENTRY *)realloc(CommandLineCache, newCapacity * sizeof(WMI_CACHE_ENTRY));
	if (!newCache) {
		return FALSE;
	}

	for (DWORD i = CommandLineCacheCapacity; i < newCapacity; i++) {
		newCache[i].Pid = 0;
		newCache[i].CommandLine = NULL;
		newCache[i].ServiceNames = NULL;
		newCache[i].Valid = FALSE;
		newCache[i].IsService = FALSE;
	}

	CommandLineCache = newCache;
	CommandLineCacheCapacity = newCapacity;
	return TRUE;
}

static BOOL EnsureServiceCacheCapacity(DWORD required)
{
	if (ServiceCacheCapacity >= required) {
		return TRUE;
	}

	DWORD newCapacity = ServiceCacheCapacity == 0 ? WMI_INITIAL_SERVICE_CACHE_CAPACITY : ServiceCacheCapacity;
	while (newCapacity < required) {
		newCapacity *= WMI_CACHE_GROWTH_FACTOR;
	}

	WMI_SERVICE_CACHE_ENTRY *newCache = (WMI_SERVICE_CACHE_ENTRY *)realloc(ServiceCache, newCapacity * sizeof(WMI_SERVICE_CACHE_ENTRY));
	if (!newCache) {
		return FALSE;
	}

	for (DWORD i = ServiceCacheCapacity; i < newCapacity; i++) {
		newCache[i].ProcessId = 0;
		newCache[i].ServiceName = NULL;
		newCache[i].Valid = FALSE;
	}

	ServiceCache = newCache;
	ServiceCacheCapacity = newCapacity;
	return TRUE;
}

static void FreeCommandLineCacheEntry(DWORD index)
{
	if (index >= CommandLineCacheCount) {
		return;
	}

	if (CommandLineCache[index].CommandLine) {
		free(CommandLineCache[index].CommandLine);
		CommandLineCache[index].CommandLine = NULL;
	}
	if (CommandLineCache[index].ServiceNames) {
		free(CommandLineCache[index].ServiceNames);
		CommandLineCache[index].ServiceNames = NULL;
	}
	CommandLineCache[index].Valid = FALSE;
}

static void FreeServiceCacheEntry(DWORD index)
{
	if (index >= ServiceCacheCount) {
		return;
	}

	if (ServiceCache[index].ServiceName) {
		free(ServiceCache[index].ServiceName);
		ServiceCache[index].ServiceName = NULL;
	}
	ServiceCache[index].Valid = FALSE;
}

static void ClearCommandLineCache(void)
{
	for (DWORD i = 0; i < CommandLineCacheCount; i++) {
		FreeCommandLineCacheEntry(i);
	}
	CommandLineCacheCount = 0;
}

static void ClearServiceCache(void)
{
	for (DWORD i = 0; i < ServiceCacheCount; i++) {
		FreeServiceCacheEntry(i);
	}
	ServiceCacheCount = 0;
}

static void FreeAllCacheMemory(void)
{
	ClearCommandLineCache();
	ClearServiceCache();

	if (CommandLineCache) {
		free(CommandLineCache);
		CommandLineCache = NULL;
	}
	CommandLineCacheCapacity = 0;
	CommandLineCacheCount = 0;

	if (ServiceCache) {
		free(ServiceCache);
		ServiceCache = NULL;
	}
	ServiceCacheCapacity = 0;
	ServiceCacheCount = 0;
}

void ReplaceChar(TCHAR* str, TCHAR find, TCHAR replace)
{
	TCHAR *current_pos = _tcschr(str, find);
	while (current_pos) {
		*current_pos = replace;
		current_pos = _tcschr(current_pos + 1, find);
	}
}

BOOL WmiInit(void)
{
	WmiThreadRunning = TRUE;
	WmiThreadHandle = CreateThread(NULL, 0, WmiCacheThreadProc, NULL, 0, NULL);
	if (!WmiThreadHandle) {
		WmiThreadRunning = FALSE;
		CloseHandle(WmiStopEvent);
		FreeAllCacheMemory();
		DeleteCriticalSection(&WmiCacheLock);
		return FALSE;
	}

	return TRUE;
}

void WmiCleanup(void)
{
	if (!WmiInitialized) {
		return;
	}

	WmiThreadRunning = FALSE;

	if (WmiStopEvent) {
		SetEvent(WmiStopEvent);
	}

	if (WmiThreadHandle) {
		WaitForSingleObject(WmiThreadHandle, 2000);
		CloseHandle(WmiThreadHandle);
		WmiThreadHandle = NULL;
	}

	if (WmiStopEvent) {
		CloseHandle(WmiStopEvent);
		WmiStopEvent = NULL;
	}

	FreeAllCacheMemory();

	DeleteCriticalSection(&WmiCacheLock);
	WmiInitialized = FALSE;
}

static DWORD WINAPI WmiCacheThreadProc(LPVOID lpParam)
{
	HRESULT hr;
	
	if (WmiInitialized) {
		return TRUE;
	}

	InitializeCriticalSection(&WmiCacheLock);

	if (!EnsureCommandLineCacheCapacity(WMI_INITIAL_PROCESS_CACHE_CAPACITY)) {
		DeleteCriticalSection(&WmiCacheLock);
		return FALSE;
	}
	if (!EnsureServiceCacheCapacity(WMI_INITIAL_SERVICE_CACHE_CAPACITY)) {
		FreeAllCacheMemory();
		DeleteCriticalSection(&WmiCacheLock);
		return FALSE;
	}

	WmiStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!WmiStopEvent) {
		FreeAllCacheMemory();
		DeleteCriticalSection(&WmiCacheLock);
		return FALSE;
	}

	hr = CoInitializeEx(0, COINIT_MULTITHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
		CloseHandle(WmiStopEvent);
		FreeAllCacheMemory();
		DeleteCriticalSection(&WmiCacheLock);
		return FALSE;
	}

	hr = CoInitializeSecurity(
		NULL,
		-1,
		NULL,
		NULL,
		RPC_C_AUTHN_LEVEL_DEFAULT,
		RPC_C_IMP_LEVEL_IMPERSONATE,
		NULL,
		EOAC_NONE,
		NULL
	);
	
	if (FAILED(hr)) {
		CoUninitialize();
		CloseHandle(WmiStopEvent);
		FreeAllCacheMemory();
		DeleteCriticalSection(&WmiCacheLock);
		return FALSE;
	}

	IWbemLocator *WmiLocator = NULL;
	hr = CoCreateInstance(
		&CLSID_WbemLocator,
		0,
		CLSCTX_INPROC_SERVER,
		&IID_IWbemLocator,
		(LPVOID *)&WmiLocator
	);

	if (FAILED(hr)) {
		CoUninitialize();
		CloseHandle(WmiStopEvent);
		FreeAllCacheMemory();
		DeleteCriticalSection(&WmiCacheLock);
		return FALSE;
	}

	IWbemServices *WmiServices = NULL;
	hr = WmiLocator->lpVtbl->ConnectServer(
		WmiLocator,
		L"root\\cimv2",
		NULL,
		NULL,
		0,
		NULL,
		NULL,
		0,
		&WmiServices
	);

	if (FAILED(hr)) {
		WmiLocator->lpVtbl->Release(WmiLocator);
		CoUninitialize();
		CloseHandle(WmiStopEvent);
		FreeAllCacheMemory();
		DeleteCriticalSection(&WmiCacheLock);
		return FALSE;
	}

	RefreshCommandLineCacheInternal(WmiServices);
	RefreshServiceCacheInternal(WmiServices);

	WmiServices->lpVtbl->Release(WmiServices);
	WmiLocator->lpVtbl->Release(WmiLocator);
	CoUninitialize();
	WmiInitialized = TRUE;

	UNREFERENCED_PARAMETER(lpParam);

	while (WmiThreadRunning && WmiInitialized) {
		DWORD waitResult = WaitForSingleObject(WmiStopEvent, WMI_CACHE_REFRESH_INTERVAL_MS);
		
		if (waitResult == WAIT_OBJECT_0) {
			break;
		}

		RefreshCommandLineCache();
	}

	return 0;
}

static void RefreshCommandLineCacheInternal(IWbemServices *WmiServices)
{
	if (!WmiServices) {
		return;
	}

	IEnumWbemClassObject *Enumerator = NULL;

	HRESULT hr = WmiServices->lpVtbl->ExecQuery(
		WmiServices,
		L"WQL",
		L"SELECT ProcessId, Name, CommandLine FROM Win32_Process",
		WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY,
		NULL,
		&Enumerator
	);

	if (FAILED(hr) || !Enumerator) {
		return;
	}

	EnterCriticalSection(&WmiCacheLock);
	ClearCommandLineCache();

	IWbemClassObject *ClassObj = NULL;
	ULONG Returned = 0;
	DWORD cacheIndex = 0;

	while (TRUE) {
		hr = Enumerator->lpVtbl->Next(Enumerator, 1000, 1, &ClassObj, &Returned);
		
		if (hr == WBEM_S_TIMEDOUT || Returned == 0) {
			break;
		}

		if (FAILED(hr) || !ClassObj) {
			break;
		}

		if (!EnsureCommandLineCacheCapacity(cacheIndex + 1)) {
			ClassObj->lpVtbl->Release(ClassObj);
			break;
		}

		VARIANT varPid;
		VariantInit(&varPid);
		hr = ClassObj->lpVtbl->Get(ClassObj, L"ProcessId", 0, &varPid, 0, 0);

		if (SUCCEEDED(hr) && varPid.vt == VT_I4) {
			DWORD pid = (DWORD)varPid.lVal;
			
			VARIANT varCmdLine;
			VariantInit(&varCmdLine);
			hr = ClassObj->lpVtbl->Get(ClassObj, L"CommandLine", 0, &varCmdLine, 0, 0);

			if (SUCCEEDED(hr) && varCmdLine.vt == VT_BSTR && varCmdLine.bstrVal != NULL) {
				CommandLineCache[cacheIndex].Pid = pid;
				
				size_t cmdLineLen = SysStringLen(varCmdLine.bstrVal) + 1;
				CommandLineCache[cacheIndex].CommandLine = (TCHAR *)malloc(cmdLineLen * sizeof(TCHAR));
				if (CommandLineCache[cacheIndex].CommandLine) {
#ifdef UNICODE
					_tcsncpy_s(CommandLineCache[cacheIndex].CommandLine, 
						cmdLineLen, varCmdLine.bstrVal, cmdLineLen - 1);
#else
					WideCharToMultiByte(CP_ACP, 0, varCmdLine.bstrVal, -1,
						CommandLineCache[cacheIndex].CommandLine, 
						cmdLineLen, NULL, NULL);
#endif
					ReplaceChar(CommandLineCache[cacheIndex].CommandLine, _T('\n'), _T(' '));
					ReplaceChar(CommandLineCache[cacheIndex].CommandLine, _T('\r'), _T(' '));
					ReplaceChar(CommandLineCache[cacheIndex].CommandLine, _T('\t'), _T(' '));
				}

				CommandLineCache[cacheIndex].ServiceNames = (TCHAR *)malloc(WMI_COMMAND_LINE_SIZE * sizeof(TCHAR));
				if (CommandLineCache[cacheIndex].ServiceNames) {
					CommandLineCache[cacheIndex].ServiceNames[0] = _T('\0');
				}

				CommandLineCache[cacheIndex].Valid = TRUE;
				CommandLineCache[cacheIndex].IsService = FALSE;
				cacheIndex++;
			} else {
				VARIANT varName;
				VariantInit(&varName);
				hr = ClassObj->lpVtbl->Get(ClassObj, L"Name", 0, &varName, 0, 0);

				if (SUCCEEDED(hr) && varName.vt == VT_BSTR && varName.bstrVal != NULL) {
					CommandLineCache[cacheIndex].Pid = pid;
					
					size_t nameLen = SysStringLen(varName.bstrVal) + 1;
					CommandLineCache[cacheIndex].CommandLine = (TCHAR *)malloc(nameLen * sizeof(TCHAR));
					if (CommandLineCache[cacheIndex].CommandLine) {
#ifdef UNICODE
						_tcsncpy_s(CommandLineCache[cacheIndex].CommandLine,
							nameLen, varName.bstrVal, nameLen - 1);
#else
						WideCharToMultiByte(CP_ACP, 0, varName.bstrVal, -1,
							CommandLineCache[cacheIndex].CommandLine,
							nameLen, NULL, NULL);
#endif
					}

					CommandLineCache[cacheIndex].ServiceNames = (TCHAR *)malloc(WMI_COMMAND_LINE_SIZE * sizeof(TCHAR));
					if (CommandLineCache[cacheIndex].ServiceNames) {
						CommandLineCache[cacheIndex].ServiceNames[0] = _T('\0');
					}

					CommandLineCache[cacheIndex].Valid = TRUE;
					CommandLineCache[cacheIndex].IsService = FALSE;
					cacheIndex++;
				}

				VariantClear(&varName);
			}

			VariantClear(&varCmdLine);
		}

		VariantClear(&varPid);
		ClassObj->lpVtbl->Release(ClassObj);
	}

	CommandLineCacheCount = cacheIndex;
	LeaveCriticalSection(&WmiCacheLock);

	Enumerator->lpVtbl->Release(Enumerator);
}

static void RefreshCommandLineCache(void)
{
	HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
		return;
	}

	hr = CoInitializeSecurity(
		NULL, -1, NULL, NULL,
		RPC_C_AUTHN_LEVEL_DEFAULT,
		RPC_C_IMP_LEVEL_IMPERSONATE,
		NULL, EOAC_NONE, NULL
	);

	if (FAILED(hr)) {
		CoUninitialize();
		return;
	}

	IWbemLocator *WmiLocator = NULL;
	hr = CoCreateInstance(
		&CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
		&IID_IWbemLocator, (LPVOID *)&WmiLocator
	);

	if (FAILED(hr) || !WmiLocator) {
		CoUninitialize();
		return;
	}

	IWbemServices *WmiServices = NULL;
	hr = WmiLocator->lpVtbl->ConnectServer(
		WmiLocator, L"root\\cimv2", NULL, NULL, 0, NULL, NULL, 0, &WmiServices
	);

	if (FAILED(hr) || !WmiServices) {
		WmiLocator->lpVtbl->Release(WmiLocator);
		CoUninitialize();
		return;
	}

	CoSetProxyBlanket(
		(IUnknown *)WmiServices,
		RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
		RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
		NULL, EOAC_NONE
	);

	RefreshCommandLineCacheInternal(WmiServices);
	RefreshServiceCacheInternal(WmiServices);

	WmiServices->lpVtbl->Release(WmiServices);
	WmiLocator->lpVtbl->Release(WmiLocator);
	CoUninitialize();
}

static void RefreshServiceCacheInternal(IWbemServices *WmiServices)
{
	if (!WmiServices) {
		return;
	}

	IEnumWbemClassObject *Enumerator = NULL;

	HRESULT hr = WmiServices->lpVtbl->ExecQuery(
		WmiServices,
		L"WQL",
		L"SELECT Name, ProcessId FROM Win32_Service WHERE State = 'Running'",
		WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY,
		NULL,
		&Enumerator
	);

	if (FAILED(hr) || !Enumerator) {
		return;
	}

	EnterCriticalSection(&WmiCacheLock);
	ClearServiceCache();

	IWbemClassObject *ClassObj = NULL;
	ULONG Returned = 0;
	DWORD serviceIndex = 0;

	while (TRUE) {
		hr = Enumerator->lpVtbl->Next(Enumerator, 1000, 1, &ClassObj, &Returned);
		
		if (hr == WBEM_S_TIMEDOUT || Returned == 0) {
			break;
		}

		if (FAILED(hr) || !ClassObj) {
			break;
		}

		if (!EnsureServiceCacheCapacity(serviceIndex + 1)) {
			ClassObj->lpVtbl->Release(ClassObj);
			break;
		}

		VARIANT varPid;
		VariantInit(&varPid);
		hr = ClassObj->lpVtbl->Get(ClassObj, L"ProcessId", 0, &varPid, 0, 0);

		if (SUCCEEDED(hr) && varPid.vt == VT_I4) {
			DWORD pid = (DWORD)varPid.lVal;

			VARIANT varName;
			VariantInit(&varName);
			hr = ClassObj->lpVtbl->Get(ClassObj, L"Name", 0, &varName, 0, 0);

			if (SUCCEEDED(hr) && varName.bstrVal != NULL && varName.vt == VT_BSTR) {
				ServiceCache[serviceIndex].ProcessId = pid;
				
				size_t svcNameLen = SysStringLen(varName.bstrVal) + 1;
				ServiceCache[serviceIndex].ServiceName = (TCHAR *)malloc(svcNameLen * sizeof(TCHAR));
				if (ServiceCache[serviceIndex].ServiceName) {
#ifdef UNICODE
					_tcsncpy_s(ServiceCache[serviceIndex].ServiceName,
						svcNameLen, varName.bstrVal, svcNameLen - 1);
#else
					WideCharToMultiByte(CP_ACP, 0, varName.bstrVal, -1,
						ServiceCache[serviceIndex].ServiceName,
						svcNameLen, NULL, NULL);
#endif
				}

				ServiceCache[serviceIndex].Valid = TRUE;
				serviceIndex++;
			}

			VariantClear(&varName);
		}

		VariantClear(&varPid);
		ClassObj->lpVtbl->Release(ClassObj);
	}

	ServiceCacheCount = serviceIndex;
	LeaveCriticalSection(&WmiCacheLock);

	Enumerator->lpVtbl->Release(Enumerator);

	EnterCriticalSection(&WmiCacheLock);
	
	for (DWORD i = 0; i < CommandLineCacheCount; i++) {
		if (!CommandLineCache[i].Valid) {
			continue;
		}

		DWORD pid = CommandLineCache[i].Pid;
		TCHAR *serviceList = (TCHAR *)malloc(WMI_COMMAND_LINE_SIZE * sizeof(TCHAR));
		if (!serviceList) {
			continue;
		}
		serviceList[0] = _T('\0');
		
		BOOL firstService = TRUE;
		int serviceCount = 0;

		for (DWORD j = 0; j < ServiceCacheCount; j++) {
			if (ServiceCache[j].Valid && ServiceCache[j].ProcessId == pid && ServiceCache[j].ServiceName) {
				if (!firstService) {
					_tcscat_s(serviceList, WMI_COMMAND_LINE_SIZE, _T(","));
				}
				_tcscat_s(serviceList, WMI_COMMAND_LINE_SIZE, ServiceCache[j].ServiceName);
				firstService = FALSE;
				serviceCount++;
			}
		}

		if (serviceCount > 0) {
			CommandLineCache[i].IsService = TRUE;
			if (CommandLineCache[i].ServiceNames) {
				free(CommandLineCache[i].ServiceNames);
			}
			CommandLineCache[i].ServiceNames = serviceList;
		} else {
			CommandLineCache[i].IsService = FALSE;
			if (CommandLineCache[i].ServiceNames) {
				CommandLineCache[i].ServiceNames[0] = _T('\0');
			}
			free(serviceList);
		}
	}

	LeaveCriticalSection(&WmiCacheLock);
}

BOOL GetCachedProcessCommandLine(DWORD Pid, TCHAR *CommandLine, DWORD CommandLineSize)
{
	BOOL found = FALSE;

	if (!WmiInitialized || !CommandLine || CommandLineSize == 0) {
		return FALSE;
	}

	EnterCriticalSection(&WmiCacheLock);

	for (DWORD i = 0; i < CommandLineCacheCount; i++) {
		if (CommandLineCache[i].Valid && CommandLineCache[i].Pid == Pid && CommandLineCache[i].CommandLine) {
			_tcsncpy_s(CommandLine, CommandLineSize, 
				CommandLineCache[i].CommandLine, CommandLineSize - 1);
			CommandLine[CommandLineSize - 1] = _T('\0');
			found = TRUE;
			break;
		}
	}

	LeaveCriticalSection(&WmiCacheLock);

	return found;
}

BOOL GetProcessCommandLine(DWORD Pid, TCHAR *CommandLine, DWORD CommandLineSize)
{
	if (!WmiInitialized || !CommandLine) {
		return FALSE;
	}

	CommandLine[0] = _T('\0');

	if (GetCachedProcessCommandLine(Pid, CommandLine, CommandLineSize)) {
		return TRUE;
	}

	return FALSE;
}

BOOL GetProcessCommandLineWithServices(DWORD Pid, TCHAR *CommandLine, DWORD CommandLineSize)
{
	if (!WmiInitialized || !CommandLine || CommandLineSize == 0) {
		return FALSE;
	}

	EnterCriticalSection(&WmiCacheLock);

	WMI_CACHE_ENTRY *foundEntry = NULL;
	for (DWORD i = 0; i < CommandLineCacheCount; i++) {
		if (CommandLineCache[i].Valid && CommandLineCache[i].Pid == Pid) {
			foundEntry = &CommandLineCache[i];
			break;
		}
	}

	if (!foundEntry || !foundEntry->CommandLine) {
		LeaveCriticalSection(&WmiCacheLock);
		return FALSE;
	}

	_tcsncpy_s(CommandLine, CommandLineSize, foundEntry->CommandLine, CommandLineSize - 1);
	CommandLine[CommandLineSize - 1] = _T('\0');

	if (foundEntry->IsService && foundEntry->ServiceNames && foundEntry->ServiceNames[0] != _T('\0')) {
		TCHAR *tempBuffer = (TCHAR *)malloc((WMI_COMMAND_LINE_SIZE) * sizeof(TCHAR));
		if (!tempBuffer) {
			LeaveCriticalSection(&WmiCacheLock);
			return TRUE;
		}
		
		memset(tempBuffer, 0, (WMI_COMMAND_LINE_SIZE) * sizeof(TCHAR));
		
		_tcsncpy_s(tempBuffer, WMI_COMMAND_LINE_SIZE, CommandLine, WMI_COMMAND_LINE_SIZE - 1);
		
		size_t currentLen = _tcslen(tempBuffer);
		if (currentLen < WMI_COMMAND_LINE_SIZE - 1) {
			tempBuffer[currentLen++] = _T('[');
			tempBuffer[currentLen] = _T('\0');
		}

		size_t svcLen = _tcslen(foundEntry->ServiceNames);
		if (currentLen + svcLen < WMI_COMMAND_LINE_SIZE - 2) {
			_tcscat_s(tempBuffer, WMI_COMMAND_LINE_SIZE, foundEntry->ServiceNames);
			currentLen += svcLen;
		}

		if (currentLen < WMI_COMMAND_LINE_SIZE - 1) {
			tempBuffer[currentLen++] = _T(']');
			tempBuffer[currentLen] = _T('\0');
		}

		_tcsncpy_s(CommandLine, CommandLineSize, tempBuffer, CommandLineSize - 1);
		CommandLine[CommandLineSize - 1] = _T('\0');
		
		free(tempBuffer);
	}

	LeaveCriticalSection(&WmiCacheLock);

	return TRUE;
}

int GetRunningServices(WMI_SERVICE_INFO *Services, int MaxServices)
{
	if (!Services || MaxServices == 0) {
		return -1;
	}

	HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
		return -1;
	}

	hr = CoInitializeSecurity(
		NULL, -1, NULL, NULL,
		RPC_C_AUTHN_LEVEL_DEFAULT,
		RPC_C_IMP_LEVEL_IMPERSONATE,
		NULL, EOAC_NONE, NULL
	);

	if (FAILED(hr)) {
		CoUninitialize();
		return -1;
	}

	IWbemLocator *WmiLocator = NULL;
	hr = CoCreateInstance(
		&CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
		&IID_IWbemLocator, (LPVOID *)&WmiLocator
	);

	if (FAILED(hr) || !WmiLocator) {
		CoUninitialize();
		return -1;
	}

	IWbemServices *WmiServices = NULL;
	hr = WmiLocator->lpVtbl->ConnectServer(
		WmiLocator, L"root\\cimv2", NULL, NULL, 0, NULL, NULL, 0, &WmiServices
	);

	if (FAILED(hr) || !WmiServices) {
		WmiLocator->lpVtbl->Release(WmiLocator);
		CoUninitialize();
		return -1;
	}

	CoSetProxyBlanket(
		(IUnknown *)WmiServices,
		RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
		RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
		NULL, EOAC_NONE
	);

	IEnumWbemClassObject *Enumerator = NULL;
	hr = WmiServices->lpVtbl->ExecQuery(
		WmiServices,
		L"WQL",
		L"SELECT Name, ProcessId, State FROM Win32_Service WHERE State = 'Running'",
		WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY,
		NULL,
		&Enumerator
	);

	if (FAILED(hr) || !Enumerator) {
		WmiServices->lpVtbl->Release(WmiServices);
		WmiLocator->lpVtbl->Release(WmiLocator);
		CoUninitialize();
		return -1;
	}

	IWbemClassObject *ClassObj = NULL;
	ULONG Returned = 0;
	int serviceCount = 0;

	while (serviceCount < MaxServices) {
		hr = Enumerator->lpVtbl->Next(Enumerator, 1000, 1, &ClassObj, &Returned);
		
		if (hr == WBEM_S_TIMEDOUT || Returned == 0) {
			break;
		}

		if (FAILED(hr) || !ClassObj) {
			break;
		}

		VARIANT varName;
		VariantInit(&varName);
		hr = ClassObj->lpVtbl->Get(ClassObj, L"Name", 0, &varName, 0, 0);

		if (SUCCEEDED(hr) && varName.vt == VT_BSTR && varName.bstrVal != NULL) {
			size_t svcNameLen = SysStringLen(varName.bstrVal) + 1;
			Services[serviceCount].ServiceName = (TCHAR *)malloc(svcNameLen * sizeof(TCHAR));
			if (Services[serviceCount].ServiceName) {
#ifdef UNICODE
				_tcsncpy_s(Services[serviceCount].ServiceName, 
					svcNameLen, varName.bstrVal, svcNameLen - 1);
#else
				WideCharToMultiByte(CP_ACP, 0, varName.bstrVal, -1,
					Services[serviceCount].ServiceName, 
					svcNameLen, NULL, NULL);
#endif
			}
			VariantClear(&varName);

			VARIANT varPid;
			VariantInit(&varPid);
			hr = ClassObj->lpVtbl->Get(ClassObj, L"ProcessId", 0, &varPid, 0, 0);

			if (SUCCEEDED(hr) && varPid.vt == VT_I4) {
				Services[serviceCount].ProcessId = (DWORD)varPid.lVal;
			} else {
				Services[serviceCount].ProcessId = 0;
			}
			VariantClear(&varPid);

			VARIANT varState;
			VariantInit(&varState);
			hr = ClassObj->lpVtbl->Get(ClassObj, L"State", 0, &varState, 0, 0);

			if (SUCCEEDED(hr) && varState.vt == VT_BSTR && varState.bstrVal != NULL) {
				size_t stateLen = SysStringLen(varState.bstrVal) + 1;
				Services[serviceCount].State = (TCHAR *)malloc(stateLen * sizeof(TCHAR));
				if (Services[serviceCount].State) {
#ifdef UNICODE
					_tcsncpy_s(Services[serviceCount].State, 
						stateLen, varState.bstrVal, stateLen - 1);
#else
					WideCharToMultiByte(CP_ACP, 0, varState.bstrVal, -1,
						Services[serviceCount].State, 
						stateLen, NULL, NULL);
#endif
				}
			} else {
				Services[serviceCount].State = (TCHAR *)malloc(32 * sizeof(TCHAR));
				if (Services[serviceCount].State) {
					_tcscpy_s(Services[serviceCount].State, 32, _T("Unknown"));
				}
			}
			VariantClear(&varState);

			serviceCount++;
		}

		ClassObj->lpVtbl->Release(ClassObj);
	}

	Enumerator->lpVtbl->Release(Enumerator);
	WmiServices->lpVtbl->Release(WmiServices);
	WmiLocator->lpVtbl->Release(WmiLocator);
	CoUninitialize();

	return serviceCount;
}
