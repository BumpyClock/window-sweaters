#pragma once

#include "AppIdentity.h"
#include <Windows.h>

namespace window_sweaters::applications {

MetadataResult QueryProcessMetadata(HANDLE process, const ProcessInstance& expected);
MetadataResult QueryProcessMetadata(const ProcessInstance& expected);
std::optional<MetadataUnavailable> ValidateProcessLifetime(HANDLE process, const ProcessInstance& expected);

}
