"""Self-tests for FD-bound temporary-directory ownership and cleanup races."""

from __future__ import annotations

import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import live_trace_cleanup
from live_trace_cleanup import OwnedTemporaryRun, cleanup_run_directory
from live_trace_common import LiveTraceError


class LiveTraceCleanupTest(unittest.TestCase):
    def test_owned_temporary_run_context_cleans_normal_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            with OwnedTemporaryRun.create("test", Path(directory)) as owner:
                owned_path = owner.path
                (owned_path / "output.json").write_text("{}", encoding="utf-8")
                self.assertTrue(owned_path.is_dir())
            self.assertFalse(owned_path.exists())

    def test_owned_cleanup_is_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            owner = OwnedTemporaryRun.create("test", Path(directory))
            cleanup_run_directory(owner)
            cleanup_run_directory(owner)

    def test_cleanup_uses_random_dirfd_quarantine(self):
        with tempfile.TemporaryDirectory() as directory:
            owner = OwnedTemporaryRun.create("test", Path(directory))
            original_rename = live_trace_cleanup._rename_noreplace
            calls = []

            def record_rename(source, destination, parent_fd):
                calls.append((source, destination, parent_fd))
                return original_rename(source, destination, parent_fd)

            with mock.patch(
                "live_trace_cleanup._rename_noreplace", side_effect=record_rename
            ):
                cleanup_run_directory(owner)

            self.assertGreaterEqual(len(calls), 1)
            source, destination, parent_fd = calls[0]
            self.assertEqual(source, owner.path.name)
            self.assertRegex(destination, r"^\.llm-trace-live-quarantine\.[0-9a-f]{32}$")
            self.assertIsInstance(parent_fd, int)

    def test_cleanup_refuses_substitution_immediately_before_atomic_rename(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            parked = temporary_parent / "parked-original"
            replacement_marker = temporary_parent / "replacement-marker-path"

            def substitute(parent_fd, source_name, quarantine_name):
                self.assertTrue(quarantine_name.startswith(".llm-trace-live-quarantine."))
                original_path = temporary_parent / source_name
                original_path.rename(parked)
                original_path.mkdir()
                marker = original_path / "replacement-marker"
                marker.write_text("replacement", encoding="utf-8")
                replacement_marker.write_text(str(marker), encoding="utf-8")

            owner = OwnedTemporaryRun.create(
                "test", temporary_parent, _test_hooks={"before_quarantine_rename": substitute}
            )
            with self.assertRaises(LiveTraceError):
                cleanup_run_directory(owner)

            marker = Path(replacement_marker.read_text(encoding="utf-8"))
            self.assertEqual(marker.read_text(encoding="utf-8"), "replacement")
            self.assertTrue(parked.is_dir())

    def test_cleanup_refuses_raced_quarantine_collision_without_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            collision_path = None

            def create_collision(parent_fd, source_name, quarantine_name):
                nonlocal collision_path
                self.assertTrue(source_name.startswith("llm-trace-live.test."))
                os.mkdir(quarantine_name, dir_fd=parent_fd)
                collision_path = temporary_parent / quarantine_name
                (collision_path / "preserve").write_text("collision", encoding="utf-8")

            owner = OwnedTemporaryRun.create(
                "test", temporary_parent,
                _test_hooks={"before_quarantine_rename": create_collision},
            )
            (owner.path / "owned").write_text("owned", encoding="utf-8")
            with self.assertRaises(LiveTraceError):
                cleanup_run_directory(owner)
            self.assertEqual((owner.path / "owned").read_text(encoding="utf-8"), "owned")
            self.assertIsNotNone(collision_path)
            self.assertEqual(
                (collision_path / "preserve").read_text(encoding="utf-8"), "collision"
            )

    def test_cleanup_preserves_occupied_restore_destination_and_quarantine(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            parked = temporary_parent / "parked-owned"
            quarantine_path = None

            def occupy_and_substitute(parent_fd, source_name, quarantine_name):
                nonlocal quarantine_path
                os.mkdir(source_name, dir_fd=parent_fd)
                (temporary_parent / source_name / "occupant").write_text(
                    "original-name occupant", encoding="utf-8"
                )
                os.rename(
                    quarantine_name, parked.name, src_dir_fd=parent_fd, dst_dir_fd=parent_fd
                )
                os.mkdir(quarantine_name, dir_fd=parent_fd)
                quarantine_path = temporary_parent / quarantine_name
                (quarantine_path / "occupant").write_text(
                    "quarantine occupant", encoding="utf-8"
                )

            owner = OwnedTemporaryRun.create(
                "test", temporary_parent,
                _test_hooks={"after_quarantine_rename": occupy_and_substitute},
            )
            with self.assertRaisesRegex(LiveTraceError, "restoration failed"):
                cleanup_run_directory(owner)
            self.assertEqual(
                (owner.path / "occupant").read_text(encoding="utf-8"),
                "original-name occupant",
            )
            self.assertIsNotNone(quarantine_path)
            self.assertEqual(
                (quarantine_path / "occupant").read_text(encoding="utf-8"),
                "quarantine occupant",
            )
            self.assertTrue(parked.is_dir())

    def test_create_refuses_replacement_between_lstat_and_child_open(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            parked = temporary_parent / "parked-created"
            replacement_path = None

            def replace_before_open(path, parent_fd, name, original_identity):
                nonlocal replacement_path
                self.assertEqual(
                    (original_identity.st_dev, original_identity.st_ino),
                    (os.lstat(path).st_dev, os.lstat(path).st_ino),
                )
                path.rename(parked)
                os.mkdir(name, dir_fd=parent_fd)
                replacement_path = temporary_parent / name
                (replacement_path / "preserve").write_text("replacement", encoding="utf-8")

            with self.assertRaises(LiveTraceError):
                OwnedTemporaryRun.create(
                    "test", temporary_parent,
                    _test_hooks={"before_child_open": replace_before_open},
                )
            self.assertTrue(parked.is_dir())
            self.assertIsNotNone(replacement_path)
            self.assertEqual(
                (replacement_path / "preserve").read_text(encoding="utf-8"), "replacement"
            )

    def test_cleanup_rechecks_identity_after_pre_rmdir_substitution(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            parked = temporary_parent / "parked-before-rmdir"
            quarantine_path = None

            def substitute_before_rmdir(parent_fd, quarantine_name):
                nonlocal quarantine_path
                os.rename(
                    quarantine_name, parked.name, src_dir_fd=parent_fd, dst_dir_fd=parent_fd
                )
                os.mkdir(quarantine_name, dir_fd=parent_fd)
                quarantine_path = temporary_parent / quarantine_name
                (quarantine_path / "preserve").write_text("replacement", encoding="utf-8")

            owner = OwnedTemporaryRun.create(
                "test", temporary_parent,
                _test_hooks={"before_final_remove": substitute_before_rmdir},
            )
            with self.assertRaises(LiveTraceError):
                cleanup_run_directory(owner)
            self.assertEqual(
                (owner.path / "preserve").read_text(encoding="utf-8"), "replacement"
            )
            self.assertTrue(parked.is_dir())
            self.assertFalse(quarantine_path.exists())

    def test_cleanup_never_follows_owned_symlink_outside_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            owner = OwnedTemporaryRun.create("test", temporary_parent)
            outside = temporary_parent / "outside"
            outside.mkdir()
            marker = outside / "preserve"
            marker.write_text("outside", encoding="utf-8")
            (owner.path / "outside-link").symlink_to(outside, target_is_directory=True)
            nested = owner.path / "nested"
            nested.mkdir()
            (nested / "owned").write_text("delete", encoding="utf-8")
            cleanup_run_directory(owner)
            self.assertEqual(marker.read_text(encoding="utf-8"), "outside")

    def test_cleanup_refuses_matching_name_without_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            unowned = Path(directory) / "llm-trace-live.test.random"
            unowned.mkdir()
            marker = unowned / "preserve"
            marker.write_text("user-owned", encoding="utf-8")
            with self.assertRaises(LiveTraceError):
                cleanup_run_directory(unowned)
            self.assertEqual(marker.read_text(encoding="utf-8"), "user-owned")

    def test_cleanup_refuses_symlink_substitution(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            owner = OwnedTemporaryRun.create("test", temporary_parent)
            parked = temporary_parent / "parked-original"
            owner.path.rename(parked)
            victim = temporary_parent / "victim"
            victim.mkdir()
            marker = victim / "preserve"
            marker.write_text("user-owned", encoding="utf-8")
            owner.path.symlink_to(victim, target_is_directory=True)
            with self.assertRaises(LiveTraceError):
                cleanup_run_directory(owner)
            self.assertTrue(owner.path.is_symlink())
            self.assertEqual(marker.read_text(encoding="utf-8"), "user-owned")
            self.assertTrue(parked.is_dir())

    def test_cleanup_refuses_inode_replacement(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary_parent = Path(directory)
            owner = OwnedTemporaryRun.create("test", temporary_parent)
            parked = temporary_parent / "parked-original"
            owner.path.rename(parked)
            owner.path.mkdir()
            marker = owner.path / "preserve"
            marker.write_text("replacement", encoding="utf-8")
            with self.assertRaises(LiveTraceError):
                cleanup_run_directory(owner)
            self.assertEqual(marker.read_text(encoding="utf-8"), "replacement")
            self.assertTrue(parked.is_dir())
