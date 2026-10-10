// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <variant>

struct HostSnapshot
{
    DWORD hostProcessId = 0;
    HWND hostWindow = nullptr;
    HWND overlayWindow = nullptr;
    HWND targetWindow = nullptr;
    DWORD targetProcessId = 0;
    uint64_t targetProcessStart = 0;
    uint64_t targetGeneration = 0;
    RECT targetBounds{};
    RECT overlayBounds{};
    bool overlayVisible = false;
    bool stopped = false;
    uint64_t trackedEntries = 0;
    uint64_t creates = 0;
    uint64_t removes = 0;
    uint64_t geometryUpdates = 0;
    uint64_t draws = 0;
    uint64_t pureMoves = 0;
    uint64_t hides = 0;
    uint64_t shows = 0;
    uint64_t reorders = 0;
    uint64_t staleActions = 0;
    uint64_t placementFailures = 0;
    uint64_t trackerCallbacks = 0;
    uint64_t trackerCoalesced = 0;
    uint64_t trackerUncertainQueries = 0;
    uint64_t trackerError = 0;
    uint64_t borderPaddingPixels = 0;
};

namespace host_probe_detail
{
class JsonReader
{
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    bool Consume(char expected)
    {
        SkipSpace();
        if (position_ < text_.size() && text_[position_] == expected)
        {
            ++position_;
            return true;
        }
        return false;
    }

    bool AtEnd()
    {
        SkipSpace();
        return position_ == text_.size();
    }

    bool PlainKey(std::string_view& key)
    {
        if (!Consume('"'))
        {
            return false;
        }
        const size_t start = position_;
        while (position_ < text_.size() && text_[position_] != '"')
        {
            if (text_[position_] == '\\' || static_cast<unsigned char>(text_[position_]) < 0x20)
            {
                return false;
            }
            ++position_;
        }
        if (position_ == text_.size())
        {
            return false;
        }
        key = text_.substr(start, position_ - start);
        ++position_;
        return true;
    }

    bool SkipValue(unsigned depth = 0)
    {
        if (depth > 32)
        {
            return false;
        }
        SkipSpace();
        if (position_ == text_.size())
        {
            return false;
        }
        switch (text_[position_])
        {
        case '{':
            ++position_;
            if (Consume('}'))
            {
                return true;
            }
            do
            {
                if (!SkipString() || !Consume(':') || !SkipValue(depth + 1))
                {
                    return false;
                }
            } while (Consume(','));
            return Consume('}');
        case '[':
            ++position_;
            if (Consume(']'))
            {
                return true;
            }
            do
            {
                if (!SkipValue(depth + 1))
                {
                    return false;
                }
            } while (Consume(','));
            return Consume(']');
        case '"':
            return SkipString();
        case 't':
            return Literal("true");
        case 'f':
            return Literal("false");
        case 'n':
            return Literal("null");
        default:
        {
            std::string_view token;
            return Number(token);
        }
        }
    }

    bool Unsigned(uint64_t& value)
    {
        std::string_view token;
        return Number(token) && token.find_first_of("-.eE") == std::string_view::npos && Convert(token, value);
    }

    bool Signed(LONG& value)
    {
        std::string_view token;
        return Number(token) && token.find_first_of(".eE") == std::string_view::npos && Convert(token, value);
    }

    bool Boolean(bool& value)
    {
        SkipSpace();
        if (Literal("true"))
        {
            value = true;
            return true;
        }
        if (Literal("false"))
        {
            value = false;
            return true;
        }
        return false;
    }

    bool Rect(RECT& rectangle)
    {
        return Consume('[') &&
            Signed(rectangle.left) && Consume(',') &&
            Signed(rectangle.top) && Consume(',') &&
            Signed(rectangle.right) && Consume(',') &&
            Signed(rectangle.bottom) && Consume(']');
    }

private:
    void SkipSpace()
    {
        while (position_ < text_.size() &&
            (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\n' || text_[position_] == '\r'))
        {
            ++position_;
        }
    }

    bool IsDigit() const
    {
        return position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9';
    }

    bool Digits()
    {
        if (!IsDigit())
        {
            return false;
        }
        while (IsDigit())
        {
            ++position_;
        }
        return true;
    }

    bool Literal(std::string_view word)
    {
        SkipSpace();
        if (text_.substr(position_, word.size()) != word)
        {
            return false;
        }
        position_ += word.size();
        return true;
    }

    bool SkipString()
    {
        if (!Consume('"'))
        {
            return false;
        }
        while (position_ < text_.size())
        {
            const char current = text_[position_++];
            if (current == '"')
            {
                return true;
            }
            if (static_cast<unsigned char>(current) < 0x20)
            {
                return false;
            }
            if (current != '\\')
            {
                continue;
            }
            if (position_ == text_.size())
            {
                return false;
            }
            const char escape = text_[position_++];
            if (escape == 'u')
            {
                for (int index = 0; index < 4; ++index, ++position_)
                {
                    if (position_ == text_.size() || !std::isxdigit(static_cast<unsigned char>(text_[position_])))
                    {
                        return false;
                    }
                }
            }
            else if (std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos)
            {
                return false;
            }
        }
        return false;
    }

    bool Number(std::string_view& token)
    {
        SkipSpace();
        const size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-')
        {
            ++position_;
        }
        if (position_ < text_.size() && text_[position_] == '0')
        {
            ++position_;
        }
        else if (!Digits())
        {
            return false;
        }
        if (position_ < text_.size() && text_[position_] == '.')
        {
            ++position_;
            if (!Digits())
            {
                return false;
            }
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E'))
        {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-'))
            {
                ++position_;
            }
            if (!Digits())
            {
                return false;
            }
        }
        token = text_.substr(start, position_ - start);
        return true;
    }

    template <typename Integer>
    static bool Convert(std::string_view token, Integer& value)
    {
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        return error == std::errc{} && end == token.data() + token.size();
    }

    std::string_view text_;
    size_t position_ = 0;
};
}

inline std::optional<HostSnapshot> ParseHostSnapshot(std::string_view json)
{
    static_assert(sizeof(uintptr_t) == sizeof(uint64_t), "HWND probe values are 64-bit");
    HostSnapshot snapshot;
    uint64_t hostProcessId = 0;
    uint64_t hostWindow = 0;
    uint64_t overlayWindow = 0;
    uint64_t targetWindow = 0;
    uint64_t targetProcessId = 0;
    struct Field
    {
        std::string_view name;
        std::variant<uint64_t*, RECT*, bool*> target;
        bool seen = false;
    };
    Field fields[] = {
        { "host_pid", &hostProcessId },
        { "host_hwnd", &hostWindow },
        { "overlay_hwnd", &overlayWindow },
        { "target_hwnd", &targetWindow },
        { "target_pid", &targetProcessId },
        { "target_process_start", &snapshot.targetProcessStart },
        { "target_generation", &snapshot.targetGeneration },
        { "target_bounds", &snapshot.targetBounds },
        { "overlay_bounds", &snapshot.overlayBounds },
        { "overlay_visible", &snapshot.overlayVisible },
        { "stopped", &snapshot.stopped },
        { "tracked_entries", &snapshot.trackedEntries },
        { "creates", &snapshot.creates },
        { "removes", &snapshot.removes },
        { "geometry_updates", &snapshot.geometryUpdates },
        { "draws", &snapshot.draws },
        { "pure_moves", &snapshot.pureMoves },
        { "hides", &snapshot.hides },
        { "shows", &snapshot.shows },
        { "reorders", &snapshot.reorders },
        { "stale_actions", &snapshot.staleActions },
        { "placement_failures", &snapshot.placementFailures },
        { "tracker_callbacks", &snapshot.trackerCallbacks },
        { "tracker_coalesced", &snapshot.trackerCoalesced },
        { "tracker_uncertain_queries", &snapshot.trackerUncertainQueries },
        { "tracker_error", &snapshot.trackerError },
        { "border_width_pixels", &snapshot.borderPaddingPixels },
    };

    host_probe_detail::JsonReader reader(json);
    if (!reader.Consume('{'))
    {
        return std::nullopt;
    }
    do
    {
        std::string_view key;
        if (!reader.PlainKey(key) || !reader.Consume(':'))
        {
            return std::nullopt;
        }
        const auto field = std::ranges::find(fields, key, &Field::name);
        if (field == std::end(fields))
        {
            if (!reader.SkipValue())
            {
                return std::nullopt;
            }
            continue;
        }
        if (field->seen)
        {
            return std::nullopt;
        }
        field->seen = true;
        bool parsed = false;
        if (auto* number = std::get_if<uint64_t*>(&field->target))
        {
            parsed = reader.Unsigned(**number);
        }
        else if (auto* rectangle = std::get_if<RECT*>(&field->target))
        {
            parsed = reader.Rect(**rectangle);
        }
        else
        {
            parsed = reader.Boolean(*std::get<bool*>(field->target));
        }
        if (!parsed)
        {
            return std::nullopt;
        }
    } while (reader.Consume(','));

    if (!reader.Consume('}') || !reader.AtEnd() ||
        !std::ranges::all_of(fields, &Field::seen) ||
        hostProcessId > MAXDWORD || targetProcessId > MAXDWORD)
    {
        return std::nullopt;
    }
    snapshot.hostProcessId = static_cast<DWORD>(hostProcessId);
    snapshot.targetProcessId = static_cast<DWORD>(targetProcessId);
    snapshot.hostWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(hostWindow));
    snapshot.overlayWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(overlayWindow));
    snapshot.targetWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(targetWindow));
    return snapshot;
}
