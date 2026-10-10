#!/usr/bin/env python3
"""Reimport or verify the authored C data from the reviewed local upstream pin."""

import argparse
import pathlib
import re
import subprocess
import sys

PIN = "a49dff52483dafdbe27f972bd271cd28d37cef1c"
HERE = pathlib.Path(__file__).resolve().parent
HEADER = (
    "// SPDX-License-Identifier: GPL-3.0-only\n"
    f"// Imported from Window Sweaters {PIN}; do not edit by hand.\n"
    "// Derived from JankyBorders by Felix Kratz; see PROVENANCE.txt and NOTICE.md.\n"
)


def source(repo, path):
    return subprocess.check_output(
        ["git", "-C", str(repo), "show", f"{PIN}:{path}"], encoding="utf-8"
    )


def require(condition, message):
    if not condition:
        raise ValueError(message)


def render(repo):
    chart_source = source(repo, "src/chart.c")
    block = chart_source.split("static const struct collection_chart k_collection[] = {\n", 1)[1]
    block = block.split("\n};", 1)[0]
    charts = re.findall(
        r'\{ "([^"]+)", \{(.*?)\},\s*\{(.*?)\} \}', block, flags=re.S
    )
    require(len(charts) == 47, "The reviewed pin must contain exactly 47 charts")
    views = []
    for index, (name, raw_rows, raw_palette) in enumerate(charts):
        rows = re.findall(r'"([^"]+)"', raw_rows)
        palette = re.findall(r"0x[0-9a-f]{8}u", raw_palette)
        require(0 < len(rows) <= 12, f"{name}: invalid row count")
        require(all(len(row) == len(rows[0]) for row in rows), f"{name}: uneven rows")
        require(all(set(row) <= set(".abcdef") for row in rows), f"{name}: invalid cells")
        require(0 < len(palette) <= 6, f"{name}: invalid palette")
        flags = ["ChartDefinedYarn"]
        if name == "atelier-messages":
            flags.append("ChartRoundDots")
        else:
            flags.append("ChartSculptedYarn")
        if name == "atelier-whatsapp":
            flags.append("ChartSolidCorners")
        if name in {"atelier-notes", "atelier-calendar"}:
            flags.append("ChartOuterCuff")
        if name in {
            "atelier-finder", "atelier-terminal", "atelier-grok",
            "atelier-granola", "atelier-illustrator", "atelier-chrome",
        }:
            flags.append("ChartFittedRepeat")
        cuff = palette[0] if "ChartOuterCuff" in flags else "0"
        views.append(
            f'    {{ "{name}", {len(rows[0])}, {len(rows)}, k_collection[{index}].yarn,\n'
            f'      {" | ".join(flags)}, 0, {cuff}, k_collection[{index}].rows }},'
        )

    app_source = source(repo, "src/apps.c")
    app_block = app_source.split("static const struct app_rule k_collection[] = {", 1)[1]
    app_block = app_block.split("\n};", 1)[0]
    rules = re.findall(r'\{ "([^"]+)",\s*(0x[0-9a-f]{8}u),\s*"([^"]+)" \}', app_block)
    rule_by_match = {match: (color, chart) for match, color, chart in rules}
    presentation = source(repo, "tests/catalogue.h")
    themes = re.findall(
        r'\{ "([^"]+)", "([^"]+)", "([^"]+)", "([^"]+)", "([^"]+)" \}',
        presentation,
    )
    require(len(themes) == 37, "The reviewed pin must contain exactly 37 themes")
    theme_views = []
    id_by_chart = {}
    ids = set()
    for name, match, chart, palette, pattern in themes:
        theme_id = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
        require(theme_id not in ids, f"Duplicate theme ID: {theme_id}")
        ids.add(theme_id)
        color, rule_chart = rule_by_match[match]
        require(chart == rule_chart, f"{name}: mismatched authored chart")
        require(chart in {entry[0] for entry in charts}, f"{name}: missing chart")
        id_by_chart.setdefault(chart, theme_id)
        theme_views.append(
            f'    {{ "{theme_id}", "{name}", "{match}", "{chart}", {color},\n'
            f'      "{palette}", "{pattern}" }},'
        )
    id_by_match = {theme[1]: re.sub(r"[^a-z0-9]+", "-", theme[0].lower()).strip("-")
                   for theme in themes}
    aliases = [
        f'    {{ "{match}", "{id_by_match.get(match, id_by_chart[chart])}" }},'
        for match, _color, chart in rules
    ]
    return {
        "AuthoredCharts.inc": HEADER + "static const struct collection_chart k_collection[] = {\n"
            + block + "\n};\n",
        "ChartViews.inc": HEADER + "static const ChartView k_chart_views[] = {\n"
            + "\n".join(views) + "\n};\n",
        "ThemeViews.inc": HEADER + "static const ThemeView k_theme_views[] = {\n"
            + "\n".join(theme_views) + "\n};\n"
            + "\nstatic const ThemeAliasView k_theme_aliases[] = {\n"
            + "\n".join(aliases) + "\n};\n",
        "NOTICE.md": source(repo, "NOTICE.md"),
        "LICENSE": source(repo, "LICENSE"),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("upstream", type=pathlib.Path, help="Local upstream Git checkout")
    parser.add_argument("--verify", action="store_true", help="Compare without changing files")
    args = parser.parse_args()
    for name, content in render(args.upstream).items():
        target = HERE / name
        if args.verify:
            require(target.exists() and target.read_text(encoding="utf-8") == content,
                    f"{name} differs from the reviewed upstream pin")
        else:
            target.write_text(content, encoding="utf-8", newline="\n")
    print(f"{'Verified' if args.verify else 'Imported'} 37 themes / 47 charts at {PIN}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
