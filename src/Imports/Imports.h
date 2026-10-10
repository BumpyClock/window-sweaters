// SPDX-License-Identifier: GPL-3.0-only
// Semantics adapted from Window Sweaters a49dff52483dafdbe27f972bd271cd28d37cef1c.
#pragma once

#include "..\Applications\AppIdentity.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace window_sweaters::imports {

inline constexpr std::size_t maximum_png_bytes = 2 * 1024 * 1024;
inline constexpr std::uint32_t maximum_chart_axis = 256;
// The source raised-yarn renderer has sixteen path groups. Base yarn uses a
// group when present; reject excess groups rather than silently dropping yarn.
inline constexpr std::size_t maximum_yarn_slots = 16;
inline constexpr std::size_t maximum_rules = 64;
inline constexpr std::size_t maximum_rules_bytes = 64 * 1024;
inline constexpr std::size_t maximum_catalogue_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t maximum_effective_charts = 128;
inline constexpr std::size_t maximum_resolution_bytes = 4 * 1024 * 1024;

enum class ErrorCode {
    InvalidRule, RulesLimit, InvalidUtf8, InvalidName, InvalidPath, RemotePath,
    ReparsePoint, NotOrdinaryFile, FileIo, FileChanged, EncodedLimit,
    InvalidPng, Dimensions, UnsupportedYarnCount, DecodeFailed, HashFailed,
    ResourceLimit, DuplicateName, DuplicateIdentity, UnknownChart,
    RevisionMismatch, RevisionOverflow, InvalidStoredChart, CatalogueLimit
};

struct Error {
    ErrorCode code;
    std::string_view reason;
    std::size_t line = 0;
    std::uint32_t native_error = 0;
};
template<class T> using Result = std::variant<T, Error>;

struct Rule {
    std::string original_prefix;
    std::uint32_t base_argb;
    std::optional<std::string> chart_name;
    bool operator==(const Rule&) const = default;
};

class ParsedRules final {
public:
    ParsedRules() = default;
    std::span<const Rule> Rules() const noexcept { return rules_; }
    bool operator==(const ParsedRules&) const = default;
private:
    friend Result<ParsedRules> ParseRules(std::string_view);
    std::vector<Rule> rules_;
};

// The whole document fails on an invalid noncomment line; no partial import.
Result<ParsedRules> ParseRules(std::string_view document);
const Rule* LongestPrefix(const ParsedRules&, std::wstring_view verified_alias);

struct Unmatched {};
// Candidates are unique and sorted by AppIdentity ordering.
struct Ambiguous { std::vector<applications::AppIdentity> candidates; };
struct Matched { applications::AppIdentity application; };
using Match = std::variant<Unmatched, Ambiguous, Matched>;
struct RuleResolution { Rule rule; Match match; };

// This explicit translation uses verified executable metadata, never titles,
// display names, PIDs, or a prefix converted into a persistent identity.
Result<std::vector<RuleResolution>> ResolveRules(
    const ParsedRules&, std::span<const applications::AppMetadata> verified_metadata);
struct BoundRule {
    applications::AppIdentity application;
    Rule rule;
};
struct RuleImportPlan {
    std::vector<RuleResolution> resolutions;
    std::vector<BoundRule> bindings;
};
// One longest-prefix winner per verified identity, with source-order ties.
// An ambiguous winning rule produces diagnostics but never a binding.
Result<RuleImportPlan> PlanRuleImport(
    const ParsedRules&, std::span<const applications::AppMetadata> verified_metadata);

struct Material {
    std::uint32_t flags = 0;
    std::uint32_t corner_argb = 0;
    std::uint32_t cuff_argb = 0;
    bool operator==(const Material&) const = default;
};

class DecodedChart final {
public:
    std::uint32_t Width() const noexcept { return width_; }
    std::uint32_t Height() const noexcept { return height_; }
    // Top-down straight literal opaque ARGB, or zero = caller's base yarn.
    std::span<const std::uint32_t> Cells() const noexcept { return cells_; }
    std::span<const std::uint8_t> EncodedPng() const noexcept { return encoded_; }
    const std::string& ContentHash() const noexcept { return content_hash_; }
private:
    friend Result<DecodedChart> DecodePng(std::span<const std::uint8_t>);
    DecodedChart() = default;
    std::uint32_t width_ = 0, height_ = 0;
    std::vector<std::uint32_t> cells_;
    std::vector<std::uint8_t> encoded_;
    std::string content_hash_;
};

Result<DecodedChart> DecodePng(std::span<const std::uint8_t> encoded);

struct LocalFileIdentity {
    std::uint64_t volume_serial;
    std::array<std::uint8_t, 16> file_id;
    bool operator==(const LocalFileIdentity&) const = default;
};
struct FileImport {
    DecodedChart chart;
    LocalFileIdentity source_identity; // Import evidence, NOT a persisted chart reference.
};
// Absolute drive paths only, fixed/removable local disks, no reparse traversal.
// Reads a bounded, stable ordinary file without writes or execution.
Result<FileImport> ImportPngFile(std::wstring_view absolute_path);

class ImportedChart final {
public:
    const std::string& Id() const noexcept { return id_; }
    const std::string& Name() const noexcept { return name_; }
    const std::string& Revision() const noexcept { return revision_; }
    std::uint64_t ContentRevision() const noexcept { return content_revision_; }
    const DecodedChart& Data() const noexcept { return data_; }
    const Material& SourceMaterial() const noexcept { return material_; }
    bool Custom() const noexcept { return true; }
    bool Generated() const noexcept { return false; }
private:
    friend struct CatalogueAccess;
    ImportedChart(std::string id, std::string name, std::string revision,
                  DecodedChart data, Material material, std::uint64_t content_revision);
    std::string id_, name_, revision_;
    DecodedChart data_;
    Material material_;
    std::uint64_t content_revision_;
};

class ImportCatalogue final {
public:
    ImportCatalogue() = default;
    const ParsedRules& PersonalRules() const noexcept { return rules_; }
    std::span<const ImportedChart> Charts() const noexcept { return charts_; }
    // Last allocated asset revision, retained even after the final removal.
    std::uint64_t RevisionHighWater() const noexcept { return revision_high_water_; }
    const ImportedChart* FindById(std::string_view) const noexcept;
    const ImportedChart* FindByName(std::string_view) const noexcept;
private:
    friend struct CatalogueAccess;
    ParsedRules rules_;
    std::vector<ImportedChart> charts_;
    std::uint64_t revision_high_water_ = 0;
};

struct AddChart { std::string name; DecodedChart data; };
struct ReplaceChart {
    std::string id;
    DecodedChart data;
    std::optional<std::string> new_name;
};
struct RemoveChart { std::string id; };
struct SetRules { ParsedRules rules; };
using Edit = std::variant<AddChart, ReplaceChart, RemoveChart, SetRules>;

// Owned candidate only. The host serializes and durably commits it before
// changing its committed catalogue or acknowledging a configuration revision.
Result<ImportCatalogue> BuildCandidate(const ImportCatalogue& committed, const Edit&);

// A host storage adapter persists these owned values, never an external path.
// Rehydration redecodes bounded bytes and verifies the normalized revision.
struct StoredChart {
    std::string id, name, revision;
    Material material;
    std::vector<std::uint8_t> encoded_png;
    std::uint64_t content_revision = 0;
};
struct StoredCatalogue {
    ParsedRules rules;
    std::vector<StoredChart> charts;
    // Absence is corrupt/old storage, never a request to infer/reset history.
    std::optional<std::uint64_t> revision_high_water;
};
Result<StoredCatalogue> StoreCatalogue(const ImportCatalogue&);
Result<ImportCatalogue> RestoreCatalogue(const StoredCatalogue&);

} // namespace window_sweaters::imports
