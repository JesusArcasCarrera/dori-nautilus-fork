#!/usr/bin/env python3

from __future__ import annotations

import configparser
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


MEDIA_TOOLS = Path(sys.argv.pop(1))
ACTIONS_DIR = Path(sys.argv.pop(1))


class ActionManifestTest(unittest.TestCase):
    def load_actions(self) -> list[tuple[Path, configparser.SectionProxy]]:
        actions = []

        for path in sorted(ACTIONS_DIR.glob("*.nemo_action.in")):
            parser = configparser.ConfigParser(interpolation=None)
            parser.optionxform = str
            parser.read(path, encoding="utf-8")
            actions.append((path, parser["Nemo Action"]))

        return actions

    def test_parameter_defaults_use_split_rows(self) -> None:
        for path, action in self.load_actions():
            if "Prompt-Default" not in action:
                continue

            with self.subTest(action=path.name):
                self.assertEqual(action.get("Prompt-Mode"), "split")
                self.assertEqual(action.get("Prompt-Display-Format", "%s").count("%s"), 1)

    def test_group_sections_are_contiguous(self) -> None:
        grouped: dict[str, list[configparser.SectionProxy]] = {}

        for _path, action in self.load_actions():
            group = action.get("Group", "")
            if group:
                grouped.setdefault(group, []).append(action)

        for group, actions in grouped.items():
            actions.sort(key=lambda action: int(action.get("Position", "0")))
            section_order = []
            for action in actions:
                section = action.get("Section", "")
                if not section_order or section != section_order[-1]:
                    section_order.append(section)

            with self.subTest(group=group):
                self.assertEqual(len(section_order), len(set(section_order)))


class JoinVideosTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("ffmpeg") is None or shutil.which("ffprobe") is None:
            raise unittest.SkipTest("ffmpeg and ffprobe are required")

    def run_command(self, command: list[str]) -> None:
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE)

    def create_video(self, path: Path, duration: float, size: str = "64x48") -> None:
        self.run_command([
            "ffmpeg", "-nostdin", "-v", "error",
            "-f", "lavfi", "-i", f"color=c=blue:s={size}:r=10",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
            "-t", str(duration), "-c:v", "ffv1", "-c:a", "pcm_s16le",
            str(path),
        ])

    def add_chapters(self, source: Path, target: Path, metadata: Path) -> None:
        metadata.write_text(
            ";FFMETADATA1\n"
            "[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=100\ntitle=Introducción\n"
            "[CHAPTER]\nTIMEBASE=1/1000\nSTART=100\nEND=300\ntitle=Detalle #1\n",
            encoding="utf-8",
        )
        self.run_command([
            "ffmpeg", "-nostdin", "-v", "error", "-i", str(source),
            "-f", "ffmetadata", "-i", str(metadata),
            "-map", "0", "-map_chapters", "1", "-c", "copy", str(target),
        ])

    def probe(self, path: Path) -> dict[str, object]:
        process = subprocess.run([
            "ffprobe", "-v", "error",
            "-show_entries", "stream=codec_type,codec_name:chapter=start_time:chapter_tags=title",
            "-of", "json", str(path),
        ], check=True, capture_output=True, text=True)
        return json.loads(process.stdout)

    def test_join_preserves_order_streams_and_nested_chapters(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            directory = Path(raw_directory)
            first = directory / "Vídeo_E01-prueba 'uno' #;.mkv"
            second_base = directory / "second-base.mkv"
            second = directory / "Vídeo_E02-segundo.mkv"
            third = directory / "Vídeo_E03-final.mkv"
            self.create_video(first, 0.3)
            self.create_video(second_base, 0.3)
            self.add_chapters(second_base, second, directory / "source.ffmetadata")
            self.create_video(third, 0.3)

            environment = dict(os.environ)
            environment["NAUTILUS_MEDIA_TOOLS_NO_NOTIFY"] = "1"
            subprocess.run([
                sys.executable, str(MEDIA_TOOLS), "join-videos",
                "--value", "Resultado capítulos", str(first), str(second), str(third),
            ], check=True, env=environment, stdout=subprocess.DEVNULL,
               stderr=subprocess.PIPE)

            output = directory / "Resultado capítulos.mkv"
            self.assertTrue(output.is_file())
            payload = self.probe(output)
            self.assertEqual(
                [(stream["codec_type"], stream["codec_name"])
                 for stream in payload["streams"]],
                [("video", "ffv1"), ("audio", "pcm_s16le")],
            )
            chapters = payload["chapters"]
            self.assertEqual(
                [chapter["tags"]["title"] for chapter in chapters],
                [
                    "Vídeo E01 prueba 'uno' #;",
                    "Vídeo E02 segundo — Introducción",
                    "Vídeo E02 segundo — Detalle #1",
                    "Vídeo E03 final",
                ],
            )
            starts = [float(chapter["start_time"]) for chapter in chapters]
            self.assertEqual(starts, sorted(starts))
            self.assertEqual(len(starts), len(set(starts)))

    def test_join_rejects_incompatible_tracks_without_output(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            directory = Path(raw_directory)
            first = directory / "first.mkv"
            second = directory / "second.mkv"
            self.create_video(first, 0.2, "64x48")
            self.create_video(second, 0.2, "80x48")
            environment = dict(os.environ)
            environment["NAUTILUS_MEDIA_TOOLS_NO_NOTIFY"] = "1"
            process = subprocess.run([
                sys.executable, str(MEDIA_TOOLS), "join-videos",
                "--value", "must-not-exist", str(first), str(second),
            ], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            self.assertNotEqual(process.returncode, 0)
            self.assertFalse((directory / "must-not-exist.mkv").exists())

    def test_extract_frames_uses_requested_decimal_rate_and_default(self) -> None:
        for requested_rate, expected_frames in (("2.5", 3), (None, 5)):
            with self.subTest(rate=requested_rate), tempfile.TemporaryDirectory() as raw_directory:
                directory = Path(raw_directory)
                source = directory / "sample.mkv"
                self.create_video(source, 1.0)
                environment = dict(os.environ)
                environment["NAUTILUS_MEDIA_TOOLS_NO_NOTIFY"] = "1"
                command = [
                    sys.executable, str(MEDIA_TOOLS), "extract-video-frames",
                ]
                if requested_rate is not None:
                    command.extend(["--value", requested_rate])
                command.append(str(source))

                subprocess.run(command, check=True, env=environment,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

                output = directory / "sample_fotogramas"
                self.assertTrue(output.is_dir())
                self.assertEqual(len(list(output.glob("fotograma_*.png"))), expected_frames)

    def test_extract_frames_rejects_invalid_rate_without_partial_output(self) -> None:
        for invalid_rate in ("", "0", "-1", "text", "nan", "inf"):
            with self.subTest(rate=invalid_rate), tempfile.TemporaryDirectory() as raw_directory:
                directory = Path(raw_directory)
                source = directory / "sample.mkv"
                self.create_video(source, 0.2)
                environment = dict(os.environ)
                environment["NAUTILUS_MEDIA_TOOLS_NO_NOTIFY"] = "1"

                process = subprocess.run([
                    sys.executable, str(MEDIA_TOOLS), "extract-video-frames",
                    "--value", invalid_rate, str(source),
                ], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

                self.assertNotEqual(process.returncode, 0)
                self.assertFalse((directory / "sample_fotogramas").exists())
                self.assertEqual(list(directory.glob(".sample-fotogramas-*")), [])


if __name__ == "__main__":
    unittest.main()
