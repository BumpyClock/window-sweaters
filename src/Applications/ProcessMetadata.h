#pragma once

#include "AppIdentity.h"
#include <Windows.h>

namespace window_sweaters::applications {

// Worker-only synchronous boundary; not suitable for discovery/geometry callbacks.
// GetProcessId/GetProcessTimes revalidate the expected process lifetime while holding a handle.
// GetApplicationUserModelId (appmodel.h) resolves supported packaged PFN!PRAID identities;
// APPMODEL_ERROR_NO_APPLICATION alone selects QueryFullProcessImageNameW(flags=0) fallback.
// Both APIs require only PROCESS_QUERY_LIMITED_INFORMATION; no package-resource/icon/title reads.
// Public sources:
// https://learn.microsoft.com/windows/win32/api/appmodel/nf-appmodel-getapplicationusermodelid
// https://learn.microsoft.com/windows/win32/api/appmodel/nf-appmodel-parseapplicationusermodelid
// https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-queryfullprocessimagenamew
// Borrowed process handle; never closed here.
MetadataResult QueryProcessMetadata(HANDLE process, const ProcessInstance& expected);
MetadataResult QueryProcessMetadata(const ProcessInstance& expected);

} // namespace window_sweaters::applications
