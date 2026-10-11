#pragma once
#include "..\Contracts\Configuration.h"
#include <memory>

namespace window_sweaters::configuration {
using CatalogueSnapshot = std::shared_ptr<const imports::ImportCatalogue>;
struct ImportCandidate {
    contracts::ImportPreferences preferences;
    CatalogueSnapshot catalogue;
};
void DescribeCatalogue(contracts::ImportPreferences&, imports::ImportCatalogue const&);
bool ValidImportState(contracts::ImportPreferences const&, imports::ImportCatalogue const&);
imports::Result<ImportCandidate> PrepareImport(
    contracts::ImportPreferences const&, CatalogueSnapshot const&, contracts::ImportMutation const&,
    std::span<const applications::AppMetadata> metadata);
}
