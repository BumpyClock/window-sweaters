#pragma once
#include "..\Applications\EligibilityPreferences.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <set>
#include <stdexcept>
#include <vector>

namespace window_sweaters::contracts {
inline constexpr std::size_t maximum_payload_bytes = 1024 * 1024;
inline constexpr std::size_t maximum_preference_bytes = 256 * 1024;
inline constexpr std::size_t maximum_running_metadata_bytes = 256 * 1024;
inline constexpr std::size_t maximum_snapshot_rows = 8192;
struct ApplicationRow {
    applications::AppIdentity identity;
    std::wstring display_name;
    applications::ApplicationPresence presence{};
    bool enabled{}, explicit_preference{};
    std::uint32_t window_count{}, process_count{};
    std::optional<applications::MetadataProvenance> provenance;
    bool operator==(ApplicationRow const&) const = default;
};
struct UnavailableApplicationRow {
    std::uint32_t window_count{};
    applications::MetadataUnavailable unavailable{};
    bool operator==(UnavailableApplicationRow const&) const = default;
};
struct ApplicationsSnapshot {
    std::uint64_t observation_generation{};
    std::uint64_t configuration_revision{};
    bool default_enabled=true;
    std::vector<ApplicationRow> applications;
    std::vector<UnavailableApplicationRow> unavailable;
    bool operator==(ApplicationsSnapshot const&) const = default;
};
inline bool EqualMutation(applications::EligibilityMutation const& a,applications::EligibilityMutation const& b) {
    if (a.defaultEnabled!=b.defaultEnabled || a.clearExceptions!=b.clearExceptions || a.edits.size()!=b.edits.size()) return false;
    for (std::size_t i=0;i<a.edits.size();++i) {
        if (a.edits[i].index()!=b.edits[i].index()) return false;
        if (auto set=std::get_if<applications::SetApplicationPreference>(&a.edits[i])) {
            auto const& other=std::get<applications::SetApplicationPreference>(b.edits[i]);
            if (set->identity!=other.identity || set->enabled!=other.enabled) return false;
        } else if (std::get<applications::RemoveApplicationPreference>(a.edits[i]).identity!=
                   std::get<applications::RemoveApplicationPreference>(b.edits[i]).identity) return false;
    }
    return true;
}
inline bool BoundedPreferences(applications::EligibilityPreferences const& preferences) noexcept {
    std::size_t bytes=8;
    if (preferences.Exceptions().size()>applications::EligibilityPreferences::MaximumExceptions) return false;
    for (auto const& [identity,enabled]:preferences.Exceptions()) {
        (void)enabled;
        auto charge=12+identity.Key().size()*sizeof(wchar_t);
        if (charge>maximum_preference_bytes-bytes) return false;
        bytes+=charge;
    }
    return true;
}
namespace payload {
class Writer {
public:
    explicit Writer(std::size_t maximum=maximum_payload_bytes):limit(maximum) {}
    const std::size_t limit;
    std::vector<std::byte> bytes;
    void Number(std::uint64_t value,std::size_t count=4) {
        if (count>limit-bytes.size()) throw std::length_error("Payload exceeds aggregate bound");
        for (std::size_t i=0;i<count;++i) bytes.push_back(std::byte((value>>(8*i))&255));
    }
    void Text(std::wstring const& text,std::size_t maximum=32767) {
        if (text.size()>maximum || text.size()*2+4>limit-bytes.size())
            throw std::length_error("Text exceeds payload bound");
        Number(text.size());
        for (wchar_t unit:text) Number(static_cast<std::uint16_t>(unit),2);
    }
    void Identity(applications::AppIdentity const& identity) {
        Number(static_cast<unsigned>(identity.Kind())); Text(identity.Key());
    }
};
class Reader {
public:
    explicit Reader(std::span<std::byte const> value):bytes(value) {}
    std::span<std::byte const> bytes;
    std::size_t offset{};
    bool Number(std::uint64_t& value,std::size_t count=4) {
        if (count>bytes.size()-offset) return false;
        value=0;
        for (std::size_t i=0;i<count;++i)
            value|=std::uint64_t(std::to_integer<unsigned>(bytes[offset++]))<<(8*i);
        return true;
    }
    bool Text(std::wstring& text,std::size_t maximum=32767) {
        std::uint64_t count{};
        if (!Number(count) || count>maximum || count>(bytes.size()-offset)/2) return false;
        text.clear(); text.reserve(static_cast<std::size_t>(count));
        for (std::size_t i=0;i<count;++i) {
            std::uint64_t unit{}; if (!Number(unit,2) || unit<32 || unit==127) return false;
            if (unit>=0xd800 && unit<=0xdbff) {
                std::uint64_t low{};
                if (++i>=count || !Number(low,2) || low<0xdc00 || low>0xdfff) return false;
                text+=static_cast<wchar_t>(unit); text+=static_cast<wchar_t>(low);
            } else {
                if (unit>=0xdc00 && unit<=0xdfff) return false;
                text+=static_cast<wchar_t>(unit);
            }
        }
        return true;
    }
    std::optional<applications::AppIdentity> Identity() {
        std::uint64_t kind{}; std::wstring key;
        if (!Number(kind) || kind>1 || !Text(key)) return {};
        auto identity=kind==0?applications::AppIdentity::FromPackagedAumid(key):
            applications::AppIdentity::FromExecutablePath(key);
        if (!identity || identity->Key()!=key) return {};
        return identity;
    }
    bool Done() const noexcept { return offset==bytes.size(); }
};
inline void Preferences(Writer& writer,applications::EligibilityPreferences const& preferences) {
    if (!BoundedPreferences(preferences)) throw std::length_error("Preferences exceed aggregate bound");
    writer.Number(preferences.DefaultEnabled()); writer.Number(preferences.Exceptions().size());
    for (auto const& [identity,enabled]:preferences.Exceptions()) {
        writer.Identity(identity); writer.Number(enabled);
    }
}
inline bool Preferences(Reader& reader,applications::EligibilityPreferences& preferences) {
    std::uint64_t enabled{},count{};
    if (!reader.Number(enabled) || enabled>1 || !reader.Number(count) ||
        count>applications::EligibilityPreferences::MaximumExceptions ||
        count>(reader.bytes.size()-reader.offset)/14) return false;
    applications::EligibilityMutation mutation;
    mutation.defaultEnabled=enabled==1; mutation.clearExceptions=true;
    for (std::size_t i=0;i<count;++i) {
        auto identity=reader.Identity();
        if (!identity || !reader.Number(enabled) || enabled>1) return false;
        mutation.edits.emplace_back(applications::SetApplicationPreference{*identity,enabled==1});
    }
    auto next=preferences.Apply(mutation);
    auto value=std::get_if<applications::EligibilityPreferences>(&next);
    if (!value || !BoundedPreferences(*value)) return false;
    preferences=std::move(*value); return true;
}
inline void Mutation(Writer& writer,applications::EligibilityMutation const& mutation) {
    writer.Number(!mutation.defaultEnabled?0:*mutation.defaultEnabled?2:1);
    writer.Number(mutation.clearExceptions); writer.Number(mutation.edits.size());
    if (mutation.edits.size()>applications::EligibilityPreferences::MaximumExceptions)
        throw std::length_error("Too many eligibility edits");
    for (auto const& edit:mutation.edits) {
        if (auto set=std::get_if<applications::SetApplicationPreference>(&edit)) {
            writer.Number(set->enabled?2:1); writer.Identity(set->identity);
        } else {
            writer.Number(0); writer.Identity(std::get<applications::RemoveApplicationPreference>(edit).identity);
        }
    }
}
inline bool Mutation(Reader& reader,applications::EligibilityMutation& mutation) {
    std::uint64_t mode{},clear{},count{};
    if (!reader.Number(mode) || mode>2 || !reader.Number(clear) || clear>1 ||
        !reader.Number(count) || count>applications::EligibilityPreferences::MaximumExceptions ||
        count>(reader.bytes.size()-reader.offset)/14) return false;
    mutation={}; if (mode) mutation.defaultEnabled=mode==2;
    mutation.clearExceptions=clear==1;
    for (std::size_t i=0;i<count;++i) {
        if (!reader.Number(mode) || mode>2) return false;
        auto identity=reader.Identity(); if (!identity) return false;
        if (mode) mutation.edits.emplace_back(applications::SetApplicationPreference{*identity,mode==2});
        else mutation.edits.emplace_back(applications::RemoveApplicationPreference{*identity});
    }
    return std::holds_alternative<applications::EligibilityPreferences>(
        applications::EligibilityPreferences{}.Apply(mutation));
}
inline void Snapshot(Writer& writer,ApplicationsSnapshot const& snapshot) {
    if (snapshot.applications.size()+snapshot.unavailable.size()>maximum_snapshot_rows)
        throw std::length_error("Too many applications");
    writer.Number(snapshot.observation_generation,8); writer.Number(snapshot.configuration_revision,8);
    writer.Number(snapshot.default_enabled); writer.Number(snapshot.applications.size());
    for (auto const& row:snapshot.applications) {
        writer.Identity(row.identity); writer.Text(row.display_name,32767);
        writer.Number(static_cast<unsigned>(row.presence)); writer.Number(row.enabled);
        writer.Number(row.explicit_preference); writer.Number(row.window_count);
        writer.Number(row.process_count); writer.Number(row.provenance?1+static_cast<unsigned>(*row.provenance):0);
    }
    writer.Number(snapshot.unavailable.size());
    for (auto const& row:snapshot.unavailable) {
        writer.Number(row.window_count); writer.Number(static_cast<unsigned>(row.unavailable.reason));
        writer.Number(static_cast<unsigned>(row.unavailable.stage)); writer.Number(row.unavailable.nativeError);
    }
}
inline bool Snapshot(Reader& reader,ApplicationsSnapshot& snapshot) {
    std::uint64_t enabled{},count{},mode{},explicit_choice{},windows{},processes{},provenance{};
    if (!reader.Number(snapshot.observation_generation,8) || !reader.Number(snapshot.configuration_revision,8) ||
        !reader.Number(enabled) || enabled>1 || !reader.Number(count) || count>maximum_snapshot_rows ||
        count>(reader.bytes.size()-reader.offset)/38) return false;
    snapshot.default_enabled=enabled==1; snapshot.applications.clear(); snapshot.unavailable.clear();
    std::set<applications::AppIdentity> identities;
    std::uint64_t total_windows=0;
    for (std::size_t i=0;i<count;++i) {
        auto identity=reader.Identity(); std::wstring label;
        if (!identity || !identities.insert(*identity).second || !reader.Text(label) ||
            !reader.Number(mode) || mode>1 || !reader.Number(enabled) || enabled>1 ||
            !reader.Number(explicit_choice) || explicit_choice>1 || !reader.Number(windows) || windows>4096 ||
            !reader.Number(processes) || processes>windows || !reader.Number(provenance) || provenance>3 ||
            (mode==0 && (!windows || !processes || label.empty() || !provenance)) ||
            (mode==1 && (windows || processes || !explicit_choice || !label.empty() || provenance))) return false;
        if (windows>4096-total_windows) return false;
        total_windows+=windows;
        snapshot.applications.push_back({*identity,std::move(label),static_cast<applications::ApplicationPresence>(mode),
            enabled==1,explicit_choice==1,static_cast<std::uint32_t>(windows),static_cast<std::uint32_t>(processes),
            provenance?std::optional{static_cast<applications::MetadataProvenance>(provenance-1)}:std::nullopt});
    }
    if (!reader.Number(count) || count>maximum_snapshot_rows-snapshot.applications.size() ||
        count>(reader.bytes.size()-reader.offset)/16) return false;
    for (std::size_t i=0;i<count;++i) {
        if (!reader.Number(windows) || !windows || windows>4096 || !reader.Number(mode) || mode>8 ||
            !reader.Number(provenance) || provenance>7 || !reader.Number(enabled)) return false;
        if (windows>4096-total_windows) return false;
        total_windows+=windows;
        snapshot.unavailable.push_back({static_cast<std::uint32_t>(windows),
            {static_cast<applications::UnavailableReason>(mode),static_cast<applications::QueryStage>(provenance),
                static_cast<std::uint32_t>(enabled)}});
    }
    return true;
}
}
}
