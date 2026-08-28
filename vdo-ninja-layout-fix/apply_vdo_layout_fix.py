#!/usr/bin/env python3
"""
Patch steveseguin/ninja-obs-plugin v1.1.65 (or a close descendant) for the
OTR manual-layout workflow.

Goals:
  * never treat the host/OBS return screen-share stream as a guest camera
  * never silently add an inbound stream to whichever OBS scene happens to be active
  * stop generic grid layout from overriding scene-specific/manual transforms by default
  * make auto-inbound sources use the plugin's own VDO.Ninja Source type instead of a raw Browser Source

The patch is intentionally narrow: it does not touch unrelated OBS sources or scene items.
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def write(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", newline="\n")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def regex_once(text: str, pattern: str, repl: str, label: str, flags: int = 0) -> str:
    out, count = re.subn(pattern, repl, text, count=1, flags=flags)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one regex match, found {count}")
    return out


def patch_common(root: Path) -> None:
    path = root / "src" / "vdoninja-common.h"
    text = read(path)
    text = replace_once(
        text,
        "\tAutoLayoutMode layoutMode = AutoLayoutMode::Grid;",
        "\t// Preserve the user's OBS scene transforms unless Grid is explicitly selected.\n"
        "\tAutoLayoutMode layoutMode = AutoLayoutMode::None;",
        "default auto-inbound layout",
    )
    write(path, text)


def patch_output(root: Path) -> None:
    path = root / "src" / "vdoninja-output.cpp"
    text = read(path)

    text = regex_once(
        text,
        r'(settings_\.autoInbound\.layoutMode\s*=\s*\n\s*static_cast<AutoLayoutMode>\(getIntSetting\("auto_inbound_layout_mode",\s*static_cast<int>\(AutoLayoutMode::)Grid(\)\)\);)',
        r'\1None\2',
        "auto-inbound persisted default layout",
        flags=re.MULTILINE,
    )

    own_ids_pattern = re.compile(
        r'\s*std::vector<std::string> ownIds\s*=\s*\{\s*'
        r'settingsSnap\.streamId\s*,\s*'
        r'hashStreamId\(settingsSnap\.streamId,\s*settingsSnap\.password,\s*settingsSnap\.salt\)\s*,\s*'
        r'hashStreamId\(settingsSnap\.streamId,\s*DEFAULT_PASSWORD,\s*settingsSnap\.salt\)\s*\}\s*;\s*'
        r'autoSceneManager_->setOwnStreamIds\(ownIds\);',
        re.MULTILINE,
    )
    replacement = r'''
            std::vector<std::string> ownIds;
            const auto addOwnStreamAliases = [&](const std::string &id) {
                if (id.empty()) {
                    return;
                }
                ownIds.push_back(id);
                ownIds.push_back(hashStreamId(id, settingsSnap.password, settingsSnap.salt));
                ownIds.push_back(hashStreamId(id, DEFAULT_PASSWORD, settingsSnap.salt));
            };

            addOwnStreamAliases(settingsSnap.streamId);
            // VDO.Ninja normally uses <stream>_ss for screen share. Some
            // director/legacy flows expose <stream>S; keep both out of inbound cameras.
            addOwnStreamAliases(settingsSnap.streamId + "_ss");
            addOwnStreamAliases(settingsSnap.streamId + "S");
            autoSceneManager_->setOwnStreamIds(ownIds);'''
    text, count = own_ids_pattern.subn(replacement, text, count=1)
    if count != 1:
        raise RuntimeError(f"own screen-share aliases: expected exactly one match, found {count}")

    write(path, text)


def patch_auto_scene_manager(root: Path) -> None:
    path = root / "src" / "vdoninja-auto-scene-manager.cpp"
    text = read(path)

    if "#include <cstring>" not in text:
        text = replace_once(text, "#include <chrono>", "#include <chrono>\n#include <cstring>", "cstring include")

    text = replace_once(
        text,
        "\tAutoLayoutMode layoutMode = AutoLayoutMode::Grid;",
        "\tAutoLayoutMode layoutMode = AutoLayoutMode::None;",
        "auto scene local layout default",
    )

    old_vars = """\tbool switchScene = false;\n\tint sourceWidth = 1920;\n\tint sourceHeight = 1080;\n\tAutoLayoutMode layoutMode = AutoLayoutMode::None;\n\tstd::string targetScene;"""
    new_vars = """\tbool switchScene = false;\n\tint sourceWidth = 1920;\n\tint sourceHeight = 1080;\n\tAutoLayoutMode layoutMode = AutoLayoutMode::None;\n\tstd::string targetScene;\n\tstd::string roomId;\n\tstd::string password;\n\tstd::string wssHost;\n\tstd::string salt;"""
    text = replace_once(text, old_vars, new_vars, "auto scene source variables")

    old_assign = """\t\tlayoutMode = settings_.layoutMode;\n\t\ttargetScene = settings_.targetScene;\n\t}\n\n\tconst std::string sourceName = sourceNameForStream(streamId);"""
    new_assign = """\t\tlayoutMode = settings_.layoutMode;\n\t\ttargetScene = settings_.targetScene;\n\t\troomId = settings_.roomId;\n\t\tpassword = settings_.password;\n\t\twssHost = settings_.wssHost;\n\t\tsalt = settings_.salt;\n\t}\n\n\t// A blank target used to fall back to the current OBS scene. That is dangerous\n\t// for production layouts: opening a scene in OBS could cause a newly discovered\n\t// participant (or return feed) to appear on-air. Require an explicit target.\n\tif (targetScene.empty()) {\n\t\t{\n\t\t\tstd::lock_guard<std::mutex> lock(stateMutex_);\n\t\t\tmanagedStreamIds_.erase(streamId);\n\t\t}\n\t\tlogInfo(\"Auto-inbound discovered stream %s, but no target scene is configured; leaving OBS scene layout untouched\",\n\t\t        streamId.c_str());\n\t\treturn;\n\t}\n\n\tconst std::string sourceName = sourceNameForStream(streamId);"""
    text = replace_once(text, old_assign, new_assign, "explicit auto-inbound target")

    old_lambda_start = """\trunOnUiThread([sourceName, sourceUrl, switchScene, sourceWidth, sourceHeight, targetScene]() {"""
    new_lambda_start = """\trunOnUiThread([sourceName, streamId, switchScene, sourceWidth, sourceHeight, targetScene, roomId, password, wssHost, salt]() {"""
    text = replace_once(text, old_lambda_start, new_lambda_start, "auto-inbound UI capture")

    old_settings = """\t\tobs_data_t *settings = obs_data_create();\n\t\tobs_data_set_string(settings, \"url\", sourceUrl.c_str());\n\t\tobs_data_set_int(settings, \"width\", sourceWidth);\n\t\tobs_data_set_int(settings, \"height\", sourceHeight);\n\t\tobs_data_set_int(settings, \"fps\", 30);\n\t\tobs_data_set_bool(settings, \"reroute_audio\", true);\n\t\tobs_data_set_bool(settings, \"restart_when_active\", false);\n\t\tobs_data_set_bool(settings, \"shutdown\", false);\n\n\t\tobs_source_t *source = obs_get_source_by_name(sourceName.c_str());\n\t\tif (source) {\n\t\t\tobs_source_update(source, settings);\n\t\t} else {\n\t\t\tsource = obs_source_create(\"browser_source\", sourceName.c_str(), settings, nullptr);\n\t\t}"""
    new_settings = """\t\tobs_data_t *settings = obs_data_create();\n\t\tobs_data_set_string(settings, \"stream_id\", streamId.c_str());\n\t\tobs_data_set_string(settings, \"room_id\", roomId.c_str());\n\t\tobs_data_set_string(settings, \"password\", password.c_str());\n\t\tobs_data_set_string(settings, \"wss_host\", wssHost.c_str());\n\t\tobs_data_set_string(settings, \"salt\", salt.c_str());\n\t\t// Keep the stable browser-backed VDO.Ninja Source path by default.\n\t\t// Native Receiver remains opt-in/experimental in the source properties.\n\t\tobs_data_set_bool(settings, \"use_native_receiver\", false);\n\t\tobs_data_set_bool(settings, \"enable_data_channel\", true);\n\t\tobs_data_set_bool(settings, \"auto_reconnect\", true);\n\t\tobs_data_set_int(settings, \"width\", sourceWidth);\n\t\tobs_data_set_int(settings, \"height\", sourceHeight);\n\n\t\tobs_source_t *source = obs_get_source_by_name(sourceName.c_str());\n\t\tif (source && std::strcmp(obs_source_get_id(source), \"vdoninja_source\") != 0) {\n\t\t\t// Replace only the auto-inbound source with this exact generated name.\n\t\t\t// This upgrades older auto-inbound Browser Sources to VDO.Ninja Source.\n\t\t\tobs_source_remove(source);\n\t\t\tobs_source_release(source);\n\t\t\tsource = nullptr;\n\t\t}\n\t\tif (source) {\n\t\t\tobs_source_update(source, settings);\n\t\t} else {\n\t\t\tsource = obs_source_create(\"vdoninja_source\", sourceName.c_str(), settings, nullptr);\n\t\t}"""
    text = replace_once(text, old_settings, new_settings, "VDO.Ninja Source auto-inbound creation")

    write(path, text)


def verify(root: Path) -> None:
    common = read(root / "src" / "vdoninja-common.h")
    output = read(root / "src" / "vdoninja-output.cpp")
    manager = read(root / "src" / "vdoninja-auto-scene-manager.cpp")

    checks = {
        "layout defaults to None": "AutoLayoutMode layoutMode = AutoLayoutMode::None;" in common,
        "screen-share _ss excluded": 'addOwnStreamAliases(settingsSnap.streamId + "_ss")' in output,
        "screen-share S excluded": 'addOwnStreamAliases(settingsSnap.streamId + "S")' in output,
        "blank target protected": "no target scene is configured; leaving OBS scene layout untouched" in manager,
        "auto-inbound uses VDO source": 'obs_source_create("vdoninja_source"' in manager,
        "raw browser creation removed": 'obs_source_create("browser_source", sourceName.c_str(), settings, nullptr)' not in manager,
    }
    failed = [name for name, ok in checks.items() if not ok]
    for name, ok in checks.items():
        print(("PASS" if ok else "FAIL") + " - " + name)
    if failed:
        raise RuntimeError("verification failed: " + ", ".join(failed))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("repo", help="Path to a ninja-obs-plugin source checkout")
    args = parser.parse_args()
    root = Path(args.repo).resolve()
    if not (root / "src" / "vdoninja-auto-scene-manager.cpp").exists():
        raise SystemExit(f"Not a ninja-obs-plugin checkout: {root}")

    patch_common(root)
    patch_output(root)
    patch_auto_scene_manager(root)
    verify(root)
    print("OTR VDO.Ninja layout-preservation patch applied successfully.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
