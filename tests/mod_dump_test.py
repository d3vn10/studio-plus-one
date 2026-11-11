"""Dump one randomly selected compiled mod into a temporary copy and check asset references.

    python tests/mod_dump_test.py <studio-plus.exe> <mods-root> [--seed 7]
    python tests/mod_dump_test.py <studio-plus.exe> <mods-root> --mod-name CosmeticsPack

The source mod is never modified. The test needs an installed mod folder containing
one or more subdirectories with either patch-marker spelling and root layout.toc files.
"""
import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("studio_plus", type=Path)
    parser.add_argument("mods_root", type=Path)
    parser.add_argument("--mod-name", help="test a known mod instead of choosing one randomly")
    parser.add_argument("--game-root", type=Path)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--keep-output", action="store_true", help="leave the temporary mod copy and dump in place")
    args = parser.parse_args()

    exe = args.studio_plus.resolve()
    mods_root = args.mods_root.resolve()
    marker_names = (".reskate-studio-patch", ".reskate_studio_patch")
    candidates = {
        path: next((name for name in marker_names if (path / name).is_file()), None)
        for path in mods_root.iterdir()
        if path.is_dir() and any((path / name).is_file() for name in marker_names)
    }
    if not candidates:
        raise AssertionError(f"No marked mod directories found under {mods_root}")
    if args.mod_name:
        selected = mods_root / args.mod_name
        if selected not in candidates:
            raise AssertionError(f"{args.mod_name} is not a marked mod directory under {mods_root}")
    else:
        selected = random.Random(args.seed).choice(list(candidates))
    marker = candidates[selected]

    temp_context = (
        nullcontext(tempfile.mkdtemp(prefix="sp-mod-dump-"))
        if args.keep_output
        else tempfile.TemporaryDirectory(prefix="sp-mod-dump-")
    )
    with temp_context as temp:
        copied_mod = Path(temp) / selected.name
        shutil.copytree(selected, copied_mod, ignore=shutil.ignore_patterns(".dump"))
        command = [os.fspath(exe), "mod", "dump", os.fspath(copied_mod)]
        if args.game_root:
            command.extend(["--game-root", os.fspath(args.game_root.resolve())])
        command.append("--json")
        process = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=600,
        )
        events = []
        for line in process.stdout.splitlines():
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                continue
        result_event = next((event for event in reversed(events) if "ok" in event), None)
        if process.returncode != 0 or result_event is None or not result_event["ok"]:
            raise AssertionError(
                f"mod dump failed for {selected.name} (exit {process.returncode}):\n"
                f"{process.stderr[-4000:]}\n{process.stdout[-4000:]}"
            )

        result = result_event["result"]
        dump = Path(result["output"])
        ebx_files = list((dump / "assets" / "ebx").rglob("*.ebx"))
        res_files = list((dump / "assets" / "res").rglob("*.res"))
        chunk_files = list((dump / "assets" / "chunks").rglob("*.chunk"))
        assert ebx_files, f"{selected.name}: dump produced no EBX files"
        assert res_files or chunk_files, f"{selected.name}: dump produced no RES or chunk files"
        assert result["ebx"] >= 1, f"{selected.name}: report contains no exported EBX"
        assert result["resources"] >= 1 or result["chunks"] >= 1, (
            f"{selected.name}: report contains no exported RES or chunk"
        )
        assert result["resolved_ebx_asset_references"] >= 1, (
            f"{selected.name}: {result['ebx_asset_references']} EBX references were found, but none resolved "
            "to an exported non-EBX asset"
        )
        reference_index = json.loads(Path(result["asset_references"]).read_text(encoding="utf-8"))
        resolved = [
            (ebx, reference)
            for ebx in reference_index["ebx"]
            for reference in ebx["references"]
            if reference["resolved"]
        ]
        assert resolved, f"{selected.name}: asset-reference index has no resolved entries"
        assert len(resolved) == result["resolved_ebx_asset_references"]
        for source_ebx, reference in resolved:
            source = dump / source_ebx["file"]
            source_cas = dump / source_ebx["cas"]["file"]
            target = reference["target"]
            target_file = dump / target["file"]
            target_cas = dump / target["cas"]["file"]
            assert source.is_file() and source_cas.is_file(), f"{selected.name}: missing mapped EBX payload"
            assert target_file.is_file() and target_cas.is_file(), f"{selected.name}: missing mapped target payload"
            assert target["kind"] in {"res", "chunk"}
            assert target["cas"]["offset"] >= 0 and target["cas"]["size"] > 0

        ebx, reference = resolved[0]
        target = reference["target"]
        raw_ebx = dump / ebx["file"]
        target_file = dump / target["file"]
        assert len(ebx["sha1"]) == 40
        assert hashlib.sha1(raw_ebx.read_bytes()).hexdigest() == ebx["sha1"], (
            f"{selected.name}: raw EBX SHA-1 does not match the mapping"
        )

        print(
            f"selected={selected.name} marker={marker} dump_output={dump} "
            f"ebx={len(ebx_files)} res={len(res_files)} "
            f"chunks={len(chunk_files)} "
            f"resolved_ebx_asset_references={result['resolved_ebx_asset_references']}"
        )
        print(
            f"reference={raw_ebx.name} sha1={ebx['sha1']} -> "
            f"{target['kind']}:{reference['id']} asset={target_file.name} cas={target['cas']['file']}"
        )
        print("mod dump integration test passed")


if __name__ == "__main__":
    main()
