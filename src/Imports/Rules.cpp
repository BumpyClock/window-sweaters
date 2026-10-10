// SPDX-License-Identifier: GPL-3.0-only
// Grammar and precedence: pinned upstream src/apps.c:103-209; see README.md.
#include "Internal.h"
#include <Windows.h>
#include <algorithm>
#include <set>

namespace window_sweaters::imports {
namespace {

bool Space(char value) {
    return value == ' ' || value == '\t' || value == '\r' ||
        value == '\n' || value == '\v' || value == '\f';
}
std::string_view Trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
        value.back() == '\r' || value.back() == '\n'))
        value.remove_suffix(1);
    return value;
}
int Hex(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}
bool Prefix(std::wstring_view alias, std::string_view prefix) {
    // Parsed prefixes are valid UTF-8 and at most 63 bytes.
    std::array<wchar_t, 64> wide{};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        prefix.data(), static_cast<int>(prefix.size()), wide.data(), static_cast<int>(wide.size()));
    return count > 0 && alias.size() >= static_cast<std::size_t>(count) &&
        CompareStringOrdinal(alias.data(), count, wide.data(), count, TRUE) == CSTR_EQUAL;
}

} // namespace

namespace detail {
bool ValidUtf8(std::string_view text) {
    if (text.empty()) return true;
    return text.size() <= maximum_rules_bytes &&
        text.find('\0') == std::string_view::npos &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
}
bool ValidChartName(std::string_view name) {
    if (name.empty() || name.size() >= 64 || !ValidUtf8(name) ||
        name == "." || name == ".." || name == "none" || name == "by-app")
        return false;
    return std::none_of(name.begin(), name.end(), [](unsigned char value) {
        return value <= 32 || value == 127 || value == '/' ||
            value == '\\' || value == ':' || value == '#' || value == '=';
    });
}
} // namespace detail

Result<ParsedRules> ParseRules(std::string_view document) {
    return detail::CheckedAllocation<ParsedRules>([&]() -> Result<ParsedRules> {
        if (document.size() > maximum_rules_bytes)
            return Error{ErrorCode::RulesLimit, "Rules document exceeds 64 KiB"};
        if (!detail::ValidUtf8(document))
            return Error{ErrorCode::InvalidUtf8, "Rules must be UTF-8 without NUL"};
        ParsedRules parsed;
        std::size_t number = 0;
        while (!document.empty()) {
            ++number;
            const auto newline = document.find('\n');
            const auto length = newline == std::string_view::npos ? document.size() : newline;
            auto line = document.substr(0, length);
            if (length + (newline == std::string_view::npos ? 0 : 1) > 511)
                return Error{ErrorCode::InvalidRule, "Rule line exceeds upstream 511-byte buffer", number};
            document.remove_prefix(length + (newline == std::string_view::npos ? 0 : 1));
            line = Trim(line);
            if (line.empty() || line.front() == '#') continue;
            if (parsed.rules_.size() == maximum_rules)
                return Error{ErrorCode::RulesLimit, "More than 64 personal rules", number};
            const auto equals = line.find('=');
            if (equals == std::string_view::npos)
                return Error{ErrorCode::InvalidRule, "Expected prefix = #RRGGBB [chart] [# comment]", number};
            const auto name = Trim(line.substr(0, equals));
            auto value = Trim(line.substr(equals + 1));
            if (name.empty() || name.size() >= 64 ||
                std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
                return Error{ErrorCode::InvalidRule, "Prefix must contain 1..63 UTF-8 bytes without controls", number};
            if (value.size() < 7 || value.front() != '#' ||
                (value.size() > 7 && !Space(value[7])))
                return Error{ErrorCode::InvalidRule, "Color must be exactly six hex RGB digits", number};
            std::uint32_t rgb = 0;
            for (std::size_t i = 1; i != 7; ++i) {
                const int digit = Hex(value[i]);
                if (digit < 0)
                    return Error{ErrorCode::InvalidRule, "Color contains a nonhex RGB digit", number};
                rgb = (rgb << 4) | static_cast<std::uint32_t>(digit);
            }
            value = Trim(value.substr(7));
            std::optional<std::string> chart;
            if (!value.empty() && value.front() != '#') {
                const auto token_end = value.find_first_of(" \t\r\n");
                const auto token = value.substr(0, token_end);
                if (!detail::ValidChartName(token))
                    return Error{ErrorCode::InvalidRule, "Invalid semantic chart name", number};
                chart = std::string(token);
                value = token_end == std::string_view::npos ? std::string_view{} : Trim(value.substr(token_end));
                if (!value.empty() && value.front() != '#')
                    return Error{ErrorCode::InvalidRule, "Unexpected text after chart name", number};
            }
            parsed.rules_.push_back({std::string(name), 0xff000000u | rgb, std::move(chart)});
        }
        return parsed;
    });
}

const Rule* LongestPrefix(const ParsedRules& parsed, std::wstring_view alias) {
    const Rule* best = nullptr;
    for (const auto& rule : parsed.Rules())
        if ((!best || rule.original_prefix.size() > best->original_prefix.size()) &&
            Prefix(alias, rule.original_prefix))
            best = &rule;
    return best;
}

Result<std::vector<RuleResolution>> ResolveRules(
    const ParsedRules& parsed, std::span<const applications::AppMetadata> metadata) {
    return detail::CheckedAllocation<std::vector<RuleResolution>>([&]()
        -> Result<std::vector<RuleResolution>> {
        if (metadata.size() > 4096)
            return Error{ErrorCode::ResourceLimit, "Verified metadata snapshot exceeds 4096 applications"};
        std::size_t retained = 0;
        for (const auto& app : metadata) {
            if (app.RetainedBytes() > maximum_resolution_bytes - retained)
                return Error{ErrorCode::ResourceLimit, "Verified metadata snapshot exceeds 4 MiB retained data"};
            retained += app.RetainedBytes();
        }
        std::vector<RuleResolution> results;
        std::size_t resolution_bytes = 0;
        for (const auto& rule : parsed.Rules()) {
            std::set<applications::AppIdentity> identities;
            for (const auto& app : metadata)
                if (app.ExecutableAlias() && Prefix(*app.ExecutableAlias(), rule.original_prefix))
                    identities.insert(app.Identity());
            std::size_t size = sizeof(RuleResolution) + rule.original_prefix.size() +
                (rule.chart_name ? rule.chart_name->size() : 0);
            for (const auto& identity : identities) {
                const auto bytes = sizeof(applications::AppIdentity) + identity.Key().size() * sizeof(wchar_t);
                if (size > maximum_resolution_bytes || bytes > maximum_resolution_bytes - size)
                    return Error{ErrorCode::ResourceLimit, "Resolved identity candidates exceed 4 MiB"};
                size += bytes;
            }
            if (size > maximum_resolution_bytes - resolution_bytes)
                return Error{ErrorCode::ResourceLimit, "Resolved identity candidates exceed 4 MiB"};
            resolution_bytes += size;
            Match match = Unmatched{};
            if (identities.size() == 1) match = Matched{*identities.begin()};
            else if (identities.size() > 1)
                match = Ambiguous{{identities.begin(), identities.end()}};
            results.push_back({rule, std::move(match)});
        }
        return results;
    });
}

Result<RuleImportPlan> PlanRuleImport(
    const ParsedRules& parsed, std::span<const applications::AppMetadata> metadata) {
    return detail::CheckedAllocation<RuleImportPlan>([&]() -> Result<RuleImportPlan> {
        auto resolved = ResolveRules(parsed, metadata);
        if (const auto* error = std::get_if<Error>(&resolved)) return *error;
        RuleImportPlan plan{std::get<std::vector<RuleResolution>>(std::move(resolved)), {}};
        std::set<applications::AppIdentity> identities;
        for (const auto& app : metadata) identities.insert(app.Identity());
        for (const auto& identity : identities) {
            const RuleResolution* winner = nullptr;
            for (const auto& resolution : plan.resolutions) {
                const auto* matched = std::get_if<Matched>(&resolution.match);
                const auto* ambiguous = std::get_if<Ambiguous>(&resolution.match);
                const bool applies = (matched && matched->application == identity) ||
                    (ambiguous && std::find(ambiguous->candidates.begin(), ambiguous->candidates.end(), identity)
                        != ambiguous->candidates.end());
                if (applies && (!winner ||
                    resolution.rule.original_prefix.size() > winner->rule.original_prefix.size()))
                    winner = &resolution;
            }
            if (winner && std::holds_alternative<Matched>(winner->match))
                plan.bindings.push_back({identity, winner->rule});
        }
        return plan;
    });
}

} // namespace window_sweaters::imports
