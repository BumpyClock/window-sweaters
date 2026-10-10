// SPDX-License-Identifier: GPL-3.0-only
// Independent expectations from upstream a49dff52483dafdbe27f972bd271cd28d37cef1c.
// Row hashes: FNV-1a-64 over literal top-down rows, each followed by '\n'.
// These fixtures are not written by ImportCatalogue.py.
#pragma once

#include <cstdint>

struct ExpectedChart {
    const char* name;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t palette[6];
    std::uint64_t rows_hash;
};

inline constexpr ExpectedChart expected_charts[] = {
    { "atelier-finder", 16, 6, { 0xffb7e4f7u, 0xff24558bu }, 0xfbe066446c52159dull },
    { "atelier-terminal", 8, 6, { 0xffa5bda0u }, 0x2dda9e286b1b6375ull },
    { "atelier-grok", 12, 6, { 0xffeeece4u, 0xff868a88u }, 0x39ce479202ffb849ull },
    { "atelier-teams", 12, 6, { 0xfff4efeeu }, 0xd0094f8708966525ull },
    { "atelier-claude", 12, 6, { 0xfff7e8c5u }, 0x08883d0e9de06e11ull },
    { "atelier-codex", 12, 6, { 0xffe6e6ceu, 0xffe9bfcbu }, 0x49074e3281909ce1ull },
    { "atelier-spotify", 12, 6, { 0xffe5d586u }, 0x2c69639e37b7ec85ull },
    { "atelier-notion", 12, 6, { 0xff494947u }, 0x2c69639e37b7ec85ull },
    { "atelier-whatsapp", 12, 6, { 0xffdde9bcu, 0xffe3a2b8u }, 0xb328cf89d93f6805ull },
    { "atelier-figma", 12, 6, { 0xffeaaf96u, 0xffdcd092u, 0xff8dbccfu, 0xffafc5a1u, 0xffded3e9u }, 0xc16823ece002ff1bull },
    { "atelier-chrome", 112, 6, { 0xffd8675bu, 0xff6ba776u, 0xff4285f4u, 0xffedcc70u }, 0xf5c51a8cd81ff125ull },
    { "atelier-paper", 8, 6, { 0xfff5f5f2u, 0xff5f8bceu }, 0x2eef03e355c33035ull },
    { "atelier-safari", 12, 6, { 0xfff6f2e8u, 0xffed7066u }, 0x328ed3db41be6b65ull },
    { "atelier-firefox", 12, 6, { 0xffff873eu, 0xffffc167u }, 0x1748361ba00d9549ull },
    { "atelier-cursor", 12, 6, { 0xffc4c2b9u }, 0xd0094f8708966525ull },
    { "atelier-slack", 12, 6, { 0xff63c5dfu, 0xff82c5a6u, 0xffedc45eu, 0xffe986a3u }, 0x57ae0a55919431a7ull },
    { "atelier-zoom", 12, 6, { 0xfff6f8f4u, 0xff9ec7f5u }, 0x26df67db7529b3c5ull },
    { "atelier-telegram", 12, 6, { 0xfff5f5ecu, 0xffaddde8u }, 0x4b73c0f18c4a07c5ull },
    { "atelier-messages", 16, 6, { 0xfff8f7e8u, 0xffb7db8du }, 0xf7fb7b711bf24655ull },
    { "atelier-mail", 12, 6, { 0xfff5f5f0u, 0xffb3d8eeu }, 0xa0378867ef382dcdull },
    { "atelier-notes", 12, 6, { 0xffefc852u, 0xfffffcf3u }, 0x41c5c1e9dc9dfab5ull },
    { "atelier-calendar", 12, 6, { 0xffe55c52u }, 0x69fee518dddb7885ull },
    { "atelier-reminders", 12, 6, { 0xff579fdeu, 0xffe5787au, 0xffeba451u, 0xffccc6bau }, 0x74d4336bec9c2025ull },
    { "atelier-music", 12, 6, { 0xfff7acc0u, 0xfffff0dbu }, 0x1748361ba00d9549ull },
    { "atelier-photos", 12, 6, { 0xffefa470u, 0xffeccb67u, 0xff95ba78u, 0xff7bbbc9u, 0xff9991c1u, 0xffd98bacu }, 0xe1652836f5fbc69eull },
    { "atelier-preview", 12, 6, { 0xfff2f4efu, 0xffaacfddu }, 0xa0ff87b565d83e41ull },
    { "atelier-word", 12, 6, { 0xfff3f1e6u, 0xff82a8deu }, 0xac2d03b826f286b9ull },
    { "atelier-excel", 12, 6, { 0xff82b99au, 0xffdbead0u }, 0xb895e4bc95626bb5ull },
    { "atelier-powerpoint", 12, 6, { 0xffeea58au, 0xfff5d9b8u }, 0x39b4af666e42dd65ull },
    { "atelier-outlook", 12, 6, { 0xff8ac6ecu, 0xfff5f5eau }, 0xcf35b1b0d6e09795ull },
    { "atelier-vscode", 12, 6, { 0xffb6e1edu, 0xff17374fu }, 0xfe9c8d54847e1285ull },
    { "atelier-photoshop", 12, 6, { 0xff58b7e9u, 0xff8cbad0u }, 0x8f8f002c103aba79ull },
    { "atelier-illustrator", 12, 6, { 0xfff5a13du, 0xfff8d2a0u }, 0x98a4b9a5419889b5ull },
    { "atelier-granola", 12, 6, { 0xff8ba66au }, 0x939fc5d277859181ull },
    { "atelier-chatgpt", 12, 6, { 0xfff6f0deu }, 0xd0094f8708966525ull },
    { "atelier-ghostty", 12, 6, { 0xffecebe4u }, 0x872903b52546e1e1ull },
    { "braid", 12, 12, { 0xfff6f0deu, 0xfff078aau }, 0x50ac5b6b3b84675dull },
    { "blockstripe", 6, 1, { 0xffffffffu }, 0xfc973f914876bb3cull },
    { "checker", 4, 4, { 0xfff2eee4u }, 0x0d88ff1dab67abedull },
    { "seedling", 6, 6, { 0xff96d6a0u }, 0x4f2123c4c0f79169ull },
    { "trim", 10, 6, { 0xffffffffu, 0xfff58220u, 0xffd61e6eu }, 0xc719c42cb0d64663ull },
    { "picnic", 12, 6, { 0xfff4e6bfu }, 0x2c69639e37b7ec85ull },
    { "ribbon", 12, 6, { 0xfff5e8cfu, 0xffe3a1b3u }, 0x49074e3281909ce1ull },
    { "posy", 12, 6, { 0xfff0d1dbu, 0xfff2dd9du }, 0x277e6bf11754b30cull },
    { "twinkle", 12, 6, { 0xfff5e6bfu }, 0x08883d0e9de06e11ull },
    { "candy-stripe", 12, 6, { 0xfff5e8cfu, 0xffe3a1b3u }, 0x4dc7f5dff299d39bull },
    { "zigzag", 12, 6, { 0xfff6f0deu }, 0xd0094f8708966525ull },
};

struct ExpectedTheme {
    const char* id;
    const char* name;
    const char* match;
    const char* chart;
    std::uint32_t base;
};

inline constexpr ExpectedTheme expected_themes[] = {
    { "finder", "Finder", "Finder", "atelier-finder", 0xff258de0u },
    { "microsoft-teams", "Microsoft Teams", "Microsoft Teams", "atelier-teams", 0xff9283c1u },
    { "claude", "Claude", "Claude", "atelier-claude", 0xffd58561u },
    { "codex", "Codex", "Codex", "atelier-chatgpt", 0xff0a84ffu },
    { "spotify", "Spotify", "Spotify", "atelier-spotify", 0xff497641u },
    { "notion", "Notion", "Notion", "atelier-notion", 0xfff5f5f2u },
    { "whatsapp", "WhatsApp", "WhatsApp", "atelier-whatsapp", 0xff6ea77bu },
    { "figma", "Figma", "Figma", "atelier-figma", 0xffad7197u },
    { "google-chrome", "Google Chrome", "Google Chrome", "atelier-chrome", 0xfff4f0e6u },
    { "paper", "Paper", "Paper", "atelier-paper", 0xff83ade8u },
    { "chatgpt", "ChatGPT", "ChatGPT", "atelier-chatgpt", 0xff0a84ffu },
    { "grok-bot", "Grok Bot", "Grok Bot", "atelier-grok", 0xff34383bu },
    { "discord", "Discord", "Discord", "checker", 0xff5865f2u },
    { "granola", "Granola", "Granola", "atelier-granola", 0xff292e2au },
    { "ghostty", "Ghostty", "ghostty", "atelier-ghostty", 0xff2b3350u },
    { "safari", "Safari", "Safari", "atelier-safari", 0xff268ed8u },
    { "firefox", "Firefox", "Firefox", "atelier-firefox", 0xff643a9au },
    { "slack", "Slack", "Slack", "atelier-slack", 0xff542a52u },
    { "zoom", "Zoom", "zoom.us", "atelier-zoom", 0xff2877ebu },
    { "telegram", "Telegram", "Telegram", "atelier-telegram", 0xff389eceu },
    { "messages", "Messages", "Messages", "atelier-messages", 0xff55af51u },
    { "mail", "Mail", "Mail", "atelier-mail", 0xff2986ceu },
    { "notes", "Notes", "Notes", "atelier-notes", 0xfff6f0d9u },
    { "calendar", "Calendar", "Calendar", "atelier-calendar", 0xfff7f3e9u },
    { "reminders", "Reminders", "Reminders", "atelier-reminders", 0xfff6f3ebu },
    { "apple-music", "Apple Music", "Music", "atelier-music", 0xffe64e70u },
    { "photos", "Photos", "Photos", "atelier-photos", 0xfff8f0dbu },
    { "preview", "Preview", "Preview", "atelier-preview", 0xff597bafu },
    { "microsoft-word", "Microsoft Word", "Microsoft Word", "atelier-word", 0xff2855a1u },
    { "microsoft-excel", "Microsoft Excel", "Microsoft Excel", "atelier-excel", 0xff28674fu },
    { "microsoft-powerpoint", "Microsoft PowerPoint", "Microsoft PowerPoint", "atelier-powerpoint", 0xffb9573du },
    { "microsoft-outlook", "Microsoft Outlook", "Microsoft Outlook", "atelier-outlook", 0xff176bb7u },
    { "vs-code", "VS Code", "Code", "atelier-vscode", 0xff237cafu },
    { "adobe-photoshop", "Adobe Photoshop", "Adobe Photoshop", "atelier-photoshop", 0xff182f45u },
    { "adobe-illustrator", "Adobe Illustrator", "Adobe Illustrator", "atelier-illustrator", 0xff4e3029u },
    { "terminal", "Terminal", "Terminal", "atelier-terminal", 0xff303c35u },
    { "cursor", "Cursor", "Cursor", "atelier-cursor", 0xff26251eu },
};
