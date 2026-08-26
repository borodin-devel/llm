"""FD-bound ownership and race-resistant cleanup for live trace run directories."""

from __future__ import annotations

import ctypes
import os
from pathlib import Path
import secrets
import stat
import sys
import tempfile

from live_verification.common import LiveTraceError


_OWNER_CREATION_TOKEN = object()
_DIRECTORY_OPEN_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
_RENAME_NOREPLACE = 1
_LIBC = ctypes.CDLL(None, use_errno=True)
_LIBC_RENAMEAT2 = getattr(_LIBC, "renameat2", None)
if _LIBC_RENAMEAT2 is not None:
    _LIBC_RENAMEAT2.argtypes = (
        ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint,
    )
    _LIBC_RENAMEAT2.restype = ctypes.c_int


def _rename_noreplace(source_name, destination_name, parent_fd):
    """Atomically rename one Linux dir-FD entry without overwriting another."""
    if type(parent_fd) is not int or parent_fd < 0:
        raise LiveTraceError(f"invalid parent directory descriptor: {parent_fd!r}")
    for name in (source_name, destination_name):
        if type(name) is not str or not name or name in {".", ".."} or "/" in name or "\0" in name:
            raise LiveTraceError(f"invalid directory entry name for renameat2: {name!r}")
    if not sys.platform.startswith("linux") or _LIBC_RENAMEAT2 is None:
        raise LiveTraceError("live trace cleanup requires Linux renameat2(RENAME_NOREPLACE)")
    ctypes.set_errno(0)
    result = _LIBC_RENAMEAT2(
        parent_fd, os.fsencode(source_name), parent_fd, os.fsencode(destination_name),
        _RENAME_NOREPLACE,
    )
    if result != 0:
        error_number = ctypes.get_errno()
        raise OSError(
            error_number, os.strerror(error_number), f"{source_name} -> {destination_name}"
        )


def _same_identity(first, second):
    return (first.st_dev, first.st_ino) == (second.st_dev, second.st_ino)


def _delete_directory_contents_fd(directory_fd):
    """Delete entries beneath an opened directory without following links."""
    for name in os.listdir(directory_fd):
        try:
            entry_identity = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        except FileNotFoundError:
            continue
        if not stat.S_ISDIR(entry_identity.st_mode):
            os.unlink(name, dir_fd=directory_fd)
            continue

        child_fd = os.open(name, _DIRECTORY_OPEN_FLAGS, dir_fd=directory_fd)
        try:
            child_identity = os.fstat(child_fd)
            if not _same_identity(entry_identity, child_identity):
                raise LiveTraceError(f"directory entry {name!r} changed before traversal")
            _delete_directory_contents_fd(child_fd)
            current_identity = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            if not _same_identity(child_identity, current_identity):
                raise LiveTraceError(f"directory entry {name!r} changed during traversal")
        finally:
            os.close(child_fd)
        os.rmdir(name, dir_fd=directory_fd)


class OwnedTemporaryRun:
    """Linux capability owning one exact temporary run directory identity.

    Random run and quarantine names provide naming and collision resistance.
    Mode-0700 directories, sticky default /tmp, retained descriptors, no-follow
    traversal, and no-replace renames protect other users' paths and reject
    accidental substitution. They provide no security guarantee against a
    malicious concurrent process with the same effective UID at any point in
    the lifecycle. Such a process is trusted from directory creation and
    acquisition through cleanup; it can race before acquisition captures the
    baseline identity and after the final identity check before rmdir.
    Randomness is not same-EUID protection.
    """

    __slots__ = (
        "path", "_name", "_parent_fd", "_child_fd", "_parent_device", "_parent_inode",
        "_device", "_inode", "_cleaned", "_test_hooks",
    )

    def __init__(
        self, path, name, parent_fd, child_fd, parent_identity, identity, test_hooks, token,
    ):
        if token is not _OWNER_CREATION_TOKEN:
            raise TypeError("OwnedTemporaryRun must be created with create()")
        self.path = path
        self._name = name
        self._parent_fd = parent_fd
        self._child_fd = child_fd
        self._parent_device = parent_identity.st_dev
        self._parent_inode = parent_identity.st_ino
        self._device = identity.st_dev
        self._inode = identity.st_ino
        self._cleaned = False
        self._test_hooks = test_hooks

    @classmethod
    def create(cls, trace_name, temporary_parent=Path("/tmp"), *, _test_hooks=None):
        """Create and take ownership of one exact directory identity."""
        safe_name = "".join(
            character if character.isalnum() or character in "-_" else "_"
            for character in str(trace_name)
        )
        if not safe_name:
            raise LiveTraceError(f"{temporary_parent}: empty temporary trace name")
        parent = Path(temporary_parent).resolve(strict=True)
        if not parent.is_dir():
            raise LiveTraceError(f"{temporary_parent}: temporary parent is not a directory")
        parent_fd = os.open(parent, _DIRECTORY_OPEN_FLAGS)
        child_fd = -1
        path = None
        original_identity = None
        test_hooks = dict(_test_hooks or {})
        try:
            parent_identity = os.fstat(parent_fd)
            path = Path(tempfile.mkdtemp(prefix=f"llm-trace-live.{safe_name}.", dir=parent))
            original_identity = os.lstat(path)
            if (
                not stat.S_ISDIR(original_identity.st_mode)
                or stat.S_IMODE(original_identity.st_mode) != 0o700
            ):
                raise LiveTraceError(f"{path}: temporary run is not a mode-0700 directory")
            name = path.name
            hook = test_hooks.get("before_child_open")
            if hook is not None:
                hook(path, parent_fd, name, original_identity)
            current_identity = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
            child_fd = os.open(name, _DIRECTORY_OPEN_FLAGS, dir_fd=parent_fd)
            child_identity = os.fstat(child_fd)
            if (
                not stat.S_ISDIR(current_identity.st_mode)
                or not _same_identity(original_identity, current_identity)
                or not _same_identity(original_identity, child_identity)
            ):
                raise LiveTraceError(f"{path}: created temporary run identity mismatch")
            return cls(
                path, name, parent_fd, child_fd, parent_identity, child_identity, test_hooks,
                _OWNER_CREATION_TOKEN,
            )
        except Exception as error:
            if child_fd >= 0:
                os.close(child_fd)
            if path is not None and original_identity is not None:
                try:
                    current_identity = os.stat(
                        path.name, dir_fd=parent_fd, follow_symlinks=False
                    )
                    if _same_identity(original_identity, current_identity):
                        os.rmdir(path.name, dir_fd=parent_fd)
                except OSError:
                    pass
            os.close(parent_fd)
            if isinstance(error, LiveTraceError):
                raise
            raise LiveTraceError(
                f"{path or temporary_parent}: cannot acquire owned run: {error}"
            ) from error

    def _run_test_hook(self, name, *arguments):
        hook = self._test_hooks.get(name)
        if hook is not None:
            hook(*arguments)

    def _close_fds(self):
        for attribute in ("_child_fd", "_parent_fd"):
            descriptor = getattr(self, attribute, -1)
            if descriptor >= 0:
                try:
                    os.close(descriptor)
                finally:
                    setattr(self, attribute, -1)

    def _quarantine_name(self):
        while True:
            name = f".llm-trace-live-quarantine.{secrets.token_hex(16)}"
            try:
                os.stat(name, dir_fd=self._parent_fd, follow_symlinks=False)
            except FileNotFoundError:
                return name

    def _restore_quarantine(self, quarantine_name):
        _rename_noreplace(quarantine_name, self._name, self._parent_fd)

    def cleanup(self):
        """Quarantine and delete only through retained matching directory FDs."""
        if self._cleaned:
            return
        if self._parent_fd < 0 or self._child_fd < 0:
            raise LiveTraceError(f"{self.path}: temporary-run ownership capability is closed")
        parent_identity = os.fstat(self._parent_fd)
        if (parent_identity.st_dev, parent_identity.st_ino) != (
            self._parent_device, self._parent_inode,
        ):
            self._close_fds()
            raise LiveTraceError(f"{self.path}: temporary parent identity mismatch")

        quarantine_name = self._quarantine_name()
        moved = False
        try:
            self._run_test_hook(
                "before_quarantine_rename", self._parent_fd, self._name, quarantine_name
            )
            _rename_noreplace(self._name, quarantine_name, self._parent_fd)
            moved = True
            self._run_test_hook(
                "after_quarantine_rename", self._parent_fd, self._name, quarantine_name
            )
            quarantine_identity = os.stat(
                quarantine_name, dir_fd=self._parent_fd, follow_symlinks=False
            )
            child_identity = os.fstat(self._child_fd)
            if (
                not stat.S_ISDIR(quarantine_identity.st_mode)
                or (quarantine_identity.st_dev, quarantine_identity.st_ino)
                != (self._device, self._inode)
                or not _same_identity(quarantine_identity, child_identity)
            ):
                raise LiveTraceError(
                    f"{self.path}: refusing cleanup after directory identity substitution"
                )

            _delete_directory_contents_fd(self._child_fd)
            self._run_test_hook("before_final_remove", self._parent_fd, quarantine_name)
            final_identity = os.stat(
                quarantine_name, dir_fd=self._parent_fd, follow_symlinks=False
            )
            if not _same_identity(child_identity, final_identity):
                raise LiveTraceError(
                    f"{self.path}: refusing cleanup after quarantine identity substitution"
                )
            os.close(self._child_fd)
            self._child_fd = -1
            os.rmdir(quarantine_name, dir_fd=self._parent_fd)
            moved = False
            self._cleaned = True
        except Exception as error:
            restoration_error = None
            if moved:
                try:
                    self._restore_quarantine(quarantine_name)
                    moved = False
                except OSError as restore_error:
                    restoration_error = restore_error
            self._close_fds()
            if restoration_error is not None:
                raise LiveTraceError(
                    f"{self.path}: cleanup refused and quarantine restoration failed: "
                    f"{restoration_error}"
                ) from error
            if isinstance(error, LiveTraceError):
                raise
            raise LiveTraceError(f"{self.path}: cleanup failed safely: {error}") from error
        finally:
            if self._cleaned:
                self._close_fds()

    def __enter__(self):
        return self

    def __exit__(self, exception_type, exception, traceback):
        self.cleanup()

    def __del__(self):
        self._close_fds()


def cleanup_run_directory(owner):
    """Remove a run directory only through its creating owner capability."""
    if type(owner) is not OwnedTemporaryRun:
        raise LiveTraceError(f"{owner}: refusing cleanup without temporary-run ownership")
    owner.cleanup()
