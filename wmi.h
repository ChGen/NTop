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

#ifndef WMI_H
#define WMI_H

#include <tchar.h>
#include <windows.h>

#define WMI_COMMAND_LINE_SIZE 256
#define WMI_CACHE_REFRESH_INTERVAL_MS 5000
#define WMI_SERVICE_NAMES_SIZE 1024

#define WMI_INITIAL_PROCESS_CACHE_CAPACITY 64
#define WMI_INITIAL_SERVICE_CACHE_CAPACITY 32
#define WMI_CACHE_GROWTH_FACTOR 2

typedef struct _WMI_SERVICE_INFO {
	TCHAR *ServiceName;
	DWORD ProcessId;
	TCHAR *State;
} WMI_SERVICE_INFO;

typedef struct _WMI_CACHE_ENTRY {
	DWORD Pid;
	TCHAR *CommandLine;
	TCHAR *ServiceNames;
	BOOL Valid;
	BOOL IsService;
} WMI_CACHE_ENTRY;

typedef struct _WMI_SERVICE_CACHE_ENTRY {
	DWORD ProcessId;
	TCHAR *ServiceName;
	BOOL Valid;
} WMI_SERVICE_CACHE_ENTRY;

BOOL WmiInit(void);
void WmiCleanup(void);
BOOL GetCachedProcessCommandLine(DWORD Pid, TCHAR *CommandLine, DWORD CommandLineSize);
BOOL GetProcessCommandLine(DWORD Pid, TCHAR *CommandLine, DWORD CommandLineSize);
int GetRunningServices(WMI_SERVICE_INFO *Services, int MaxServices);
BOOL GetProcessCommandLineWithServices(DWORD Pid, TCHAR *CommandLine, DWORD CommandLineSize);

#endif
