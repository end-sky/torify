#!/usr/bin/env python3
"""Optimized Linux transparent Tor controller and CLI (Python standard library only)."""
from __future__ import annotations

import os
import pwd
import shutil
import stat
import subprocess
import time
import functools
import concurrent.futures
from pathlib import Path
from typing import Sequence, Optional, Tuple, Dict

TABLE_FAMILY = "inet"
TABLE_NAME = "torify_app"
SOCKS_PORT = 19500
TRANS_PORT = 19540
DNS_PORT = 19553
BOOTSTRAP_TIMEOUT = 180

APP_UID = os.getuid()
DATA_DIR = Path(f"/var/tmp/torify-prototype-{APP_UID}")
TORRC = Path(f"/tmp/torify-prototype-{APP_UID}.torrc")
RULES_FILE = Path(f"/tmp/torify-prototype-{APP_UID}.rules.nft")

# Cache for expensive lookups
_BINARY_CACHE: Dict[str, str] = {}
_TOR_ACCOUNT_CACHE: Optional[Tuple[int, int, str]] = None


@functools.lru_cache(maxsize=32)
def _settings_dir() -> Path:
    explicit = os.environ.get("TORIFY_CONFIG_DIR")
    if explicit:
        path = Path(explicit).expanduser()
        if not path.is_absolute():
            raise RuntimeError("TORIFY_CONFIG_DIR must be an absolute path.")
        # Security: ensure it's not a symlink that could be exploited
        try:
            if path.exists() and path.is_symlink():
                raise RuntimeError("TORIFY_CONFIG_DIR cannot be a symlink.")
        except OSError:
            pass
        return path
    config_home = os.environ.get("XDG_CONFIG_HOME")
    if config_home:
        config_path = Path(config_home).expanduser()
        if not config_path.is_absolute():
            raise RuntimeError("XDG_CONFIG_HOME must be an absolute path.")
        try:
            if config_path.exists() and config_path.is_symlink():
                raise RuntimeError("XDG_CONFIG_HOME cannot be a symlink.")
        except OSError:
            pass
        return config_path / "torify"
    home = Path.home()
    return home / ".config" / "torify"


@functools.lru_cache(maxsize=1)
def _rust_binary() -> Optional[str]:
    override = os.environ.get("TORIFY_RUST_BIN")
    if override:
        try:
            if os.path.isfile(override) and os.access(override, os.X_OK):
                # Basic security check
                if os.path.islink(override):
                    return None
                return override
        except OSError:
            pass
    
    found = shutil.which("torify-rs")
    if found:
        return found
    
    project_root = Path(__file__).resolve().parent.parent
    for candidate in (project_root / "rust" / "target" / "release" / "torify-rs",
                      project_root / "rust" / "target" / "debug" / "torify-rs"):
        try:
            if candidate.is_file() and os.access(candidate, os.X_OK) and not os.path.islink(candidate):
                return str(candidate)
        except OSError:
            continue
    return None


def _rust_env() -> dict[str, str]:
    env = os.environ.copy()
    env["TORIFY_CONFIG_DIR"] = str(_settings_dir())
    # Prevent potentially dangerous env vars
    env.pop('LD_PRELOAD', None)
    return env


def _run_rust(*args: str, capture: bool = True, timeout: float = 30) -> subprocess.CompletedProcess[str]:
    executable = _rust_binary()
    if executable is None:
        raise RuntimeError("The Rust bridge/transport helper `torify-rs` was not found. Build `rust/` with cargo and put its target/release directory on PATH.")
    
    try:
        return subprocess.run(
            [executable, *args], 
            text=True, 
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE if capture else None, 
            check=False, 
            env=_rust_env(),
            timeout=timeout
        )
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"Rust helper command timed out: {args}")


def _transport_config() -> str:
    executable = _rust_binary()
    settings = _settings_dir()
    has_saved_settings = False
    try:
        has_saved_settings = (settings / "transport").exists() or (settings / "bridges.txt").exists()
    except OSError:
        pass
    
    if executable is None:
        if has_saved_settings:
            raise RuntimeError("Bridge/transport settings exist but `torify-rs` is unavailable. Build the Rust helper before connecting.")
        return ""  # preserve simple direct-Tor operation without Rust configured
    
    result = _run_rust("config")
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "unknown Rust configuration error").strip()
        # Limit detail length for security
        if len(detail) > 500:
            detail = detail[:500] + "..."
        raise RuntimeError(f"Bridge/transport configuration failed: {detail}")
    return (result.stdout or "").strip()


@functools.lru_cache(maxsize=1)
def find_tor_account() -> Optional[Tuple[int, int, str]]:
    """Use a Tor-specific account so unrelated processes cannot bypass the firewall."""
    for name in ("tor", "debian-tor", "_tor", "toranon", "torify-tor"):
        try:
            pw = pwd.getpwnam(name)
            if pw.pw_uid != 0 and pw.pw_uid >= 100:  # Avoid system accounts
                return pw.pw_uid, pw.pw_gid, pw.pw_name
        except KeyError:
            continue
    return None


# Initialize Tor account info
TOR_ACCOUNT_INFO = find_tor_account()
# Safe placeholder values only; Connect refuses to run until a dedicated account exists.
TOR_UID, TOR_GID, TOR_ACCOUNT = TOR_ACCOUNT_INFO or (65534, 65534, "unavailable")


def binary(name: str) -> str:
    if name in _BINARY_CACHE:
        return _BINARY_CACHE[name]
    
    found = shutil.which(name)
    if not found:
        raise RuntimeError(f"Required command '{name}' was not found in PATH.")
    
    _BINARY_CACHE[name] = found
    return found


TOR_BIN = None
NFT_BIN = None
SUDO_BIN = None


def _command(argv: Sequence[str], *, input_text: str | None = None,
             capture: bool = True, timeout: float | None = 30) -> subprocess.CompletedProcess[str]:
    """Run a command with root privileges when needed; sudo must be pre-authenticated."""
    cmd = list(argv)
    if not cmd:
        raise RuntimeError("Empty command")
    
    if os.geteuid() != 0:
        sudo = SUDO_BIN or shutil.which("sudo")
        if not sudo:
            raise RuntimeError("sudo was not found. Run as root or install/configure sudo.")
        cmd = [sudo, "-n", *cmd]
    
    try:
        return subprocess.run(
            cmd,
            input=input_text,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE if capture else None,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"Command timed out: {cmd[0] if cmd else 'unknown'}")


def _checked(argv: Sequence[str], *, input_text: str | None = None,
             capture: bool = True, timeout: float | None = 30) -> str:
    result = _command(argv, input_text=input_text, capture=capture, timeout=timeout)
    if result.returncode != 0:
        stderr = (result.stderr or "").strip()
        stdout = (result.stdout or "").strip()
        detail = stderr or stdout or f"exit status {result.returncode}"
        if len(detail) > 800:
            detail = detail[:800] + "..."
        raise RuntimeError(f"Command failed: {argv[0]} ({detail})")
    return result.stdout or ""


def _sudo_authenticate() -> None:
    global SUDO_BIN
    if os.geteuid() == 0:
        return
    SUDO_BIN = shutil.which("sudo")
    if not SUDO_BIN:
        raise RuntimeError("sudo is required for firewall changes. Run the app as root or install sudo.")
    # Keep terminal attached so sudo can ask for a password.
    try:
        result = subprocess.run([SUDO_BIN, "-v"], stdin=None, stdout=None, stderr=None, check=False, timeout=60)
        if result.returncode != 0:
            raise RuntimeError("Could not obtain administrator permission. Start the app from a terminal so sudo can prompt.")
    except subprocess.TimeoutExpired:
        raise RuntimeError("sudo authentication timed out. Start the app from a terminal so sudo can prompt.")


def _safe_write(path: Path, contents: str, mode: int = 0o644) -> None:
    """Create a predictable file without following symlinks."""
    try:
        st = os.lstat(path)
        if stat.S_ISLNK(st.st_mode):
            raise RuntimeError(f"Refusing to overwrite a symlink: {path}")
        if not stat.S_ISREG(st.st_mode):
            raise RuntimeError(f"Refusing to overwrite a non-regular path: {path}")
        if st.st_uid not in {APP_UID, 0}:
            raise RuntimeError(f"Refusing to overwrite a file not owned by this user: {path}")
        os.unlink(path)
    except FileNotFoundError:
        pass
    
    # Ensure parent directory is safe
    parent = path.parent
    try:
        pst = os.lstat(parent)
        if stat.S_ISLNK(pst.st_mode):
            raise RuntimeError(f"Refusing to use a symlinked directory: {parent}")
    except FileNotFoundError:
        parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(parent, 0o700)
    
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        fd = os.open(path, flags, mode)
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(contents)
            stream.flush()
            os.fchmod(stream.fileno(), mode)
    except OSError as e:
        raise RuntimeError(f"Failed to safely write {path}: {e}")


def _tor_config() -> str:
    extra = _transport_config()
    base = f"""# Generated by Torify. Do not add secrets here.
DataDirectory {DATA_DIR}
PidFile {DATA_DIR / 'tor.pid'}
RunAsDaemon 1
SocksPort 127.0.0.1:{SOCKS_PORT}
SocksPort [::1]:{SOCKS_PORT}
TransPort 127.0.0.1:{TRANS_PORT}
TransPort [::1]:{TRANS_PORT}
DNSPort 127.0.0.1:{DNS_PORT}
DNSPort [::1]:{DNS_PORT}
SafeSocks 1
AutomapHostsOnResolve 1
VirtualAddrNetworkIPv4 10.192.0.0/10
Log notice file {DATA_DIR / 'tor.log'}
"""
    return base + (extra + "\n" if extra else "")


def _nft_rules() -> str:
    return f"""table {TABLE_FAMILY} {TABLE_NAME} {{
    chain output_nat {{
        type nat hook output priority -100; policy accept;
        meta skuid {TOR_UID} return
        oifname "lo" return
        udp dport 53 redirect to :{DNS_PORT}
        tcp dport 53 redirect to :{DNS_PORT}
        ip daddr {{ 0.0.0.0/8, 10.0.0.0/8, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.168.0.0/16, 224.0.0.0/4, 255.255.255.255 }} return
        ip6 daddr {{ ::1, fc00::/7, fe80::/10, ff00::/8 }} return
        meta l4proto tcp redirect to :{TRANS_PORT}
    }}
    chain output_guard {{
        type filter hook output priority 0; policy drop;
        meta skuid {TOR_UID} accept
        oifname "lo" accept
        ip daddr {{ 0.0.0.0/8, 10.0.0.0/8, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.168.0.0/16, 224.0.0.0/4, 255.255.255.255 }} accept
        ip6 daddr {{ ::1, fc00::/7, fe80::/10, ff00::/8 }} accept
        udp sport 68 udp dport 67 accept
        udp sport 546 udp dport 547 accept
        meta l4proto ipv6-icmp icmpv6 type {{ nd-router-solicit, nd-neighbor-solicit, nd-neighbor-advert, nd-router-advert }} accept
    }}
}}
"""


def _table_exists() -> bool:
    assert NFT_BIN
    try:
        result = _command([NFT_BIN, "list", "table", TABLE_FAMILY, TABLE_NAME], timeout=10)
        return result.returncode == 0
    except (RuntimeError, subprocess.TimeoutExpired):
        return False


def _pidfile() -> Path:
    return DATA_DIR / "tor.pid"


def _read_pid() -> Optional[int]:
    try:
        result = _command([binary("cat"), str(_pidfile())], timeout=10)
        if result.returncode != 0:
            return None
        value = (result.stdout or "").strip()
        if not value.isdecimal():
            return None
        pid = int(value)
        return pid if pid > 1 else None
    except (RuntimeError, subprocess.TimeoutExpired):
        return None


def _process_matches(pid: int) -> bool:
    if pid <= 1:
        return False
    try:
        owner = _checked([binary("stat"), "-c", "%u", f"/proc/{pid}"], timeout=10).strip()
        exe = _checked([binary("readlink"), f"/proc/{pid}/exe"], timeout=10).strip()
    except (RuntimeError, OSError, subprocess.TimeoutExpired):
        return False
    exe_name = Path(exe.removesuffix(" (deleted)")).name
    return owner == str(TOR_UID) and exe_name == "tor"


def _wait_for_bootstrap() -> bool:
    deadline = time.monotonic() + BOOTSTRAP_TIMEOUT
    log_path = str(DATA_DIR / "tor.log")
    grep = binary("grep")
    pid_missing_since: Optional[float] = None
    while time.monotonic() < deadline:
        try:
            result = _command([grep, "-Fq", "Bootstrapped 100%", log_path], timeout=10)
            if result.returncode == 0:
                return True
        except (RuntimeError, subprocess.TimeoutExpired):
            pass
        
        pid = _read_pid()
        if pid is None or not _process_matches(pid):
            if pid_missing_since is None:
                pid_missing_since = time.monotonic()
            elif time.monotonic() - pid_missing_since > 15:
                return False
        else:
            pid_missing_since = None
        time.sleep(2)
    return False


def _log_tail() -> str:
    try:
        result = _command([binary("tail"), "-n", "20", str(DATA_DIR / "tor.log")], timeout=10)
        return (result.stdout or result.stderr or "").strip()
    except (RuntimeError, subprocess.TimeoutExpired):
        return ""


def _prepare_tor() -> None:
    assert TOR_BIN
    _safe_write(TORRC, _tor_config(), 0o600)
    try:
        _checked([binary("chown"), f"{APP_UID}:{TOR_GID}", str(TORRC)], timeout=10)
        _checked([binary("chmod"), "640", str(TORRC)], timeout=10)
    except RuntimeError:
        pass  # May fail if not root, but we'll try
    
    try:
        st = os.lstat(DATA_DIR)
        if stat.S_ISLNK(st.st_mode) or not stat.S_ISDIR(st.st_mode):
            raise RuntimeError(f"Refusing to use a non-directory Tor data path: {DATA_DIR}")
    except FileNotFoundError:
        pass
    
    install = binary("install")
    try:
        _checked([install, "-d", "-o", str(TOR_UID), "-g", str(TOR_GID), "-m", "700", str(DATA_DIR)])
        _checked([binary("chown"), "-R", f"{TOR_UID}:{TOR_GID}", str(DATA_DIR)], timeout=30)
        _checked([binary("chmod"), "700", str(DATA_DIR)], timeout=10)
    except RuntimeError:
        # Fallback
        os.makedirs(DATA_DIR, mode=0o700, exist_ok=True)
        try:
            os.chown(DATA_DIR, TOR_UID, TOR_GID)
            os.chmod(DATA_DIR, 0o700)
        except OSError:
            pass
    
    try:
        _command([binary("rm"), "-f", str(_pidfile())], timeout=10)
    except (RuntimeError, subprocess.TimeoutExpired):
        pass
    _verify_tor_config()


def _service_command(argv: Sequence[str]) -> list[str]:
    if os.geteuid() == 0:
        setpriv = shutil.which("setpriv")
        sudo = SUDO_BIN or shutil.which("sudo")
        if setpriv:
            return [setpriv, f"--reuid={TOR_UID}", f"--regid={TOR_GID}", "--clear-groups", *argv]
        if sudo:
            return [sudo, "-u", f"#{TOR_UID}", "-g", f"#{TOR_GID}", *argv]
        raise RuntimeError("Cannot drop privileges for Tor: install util-linux (setpriv) or sudo.")
    sudo = SUDO_BIN or shutil.which("sudo")
    if not sudo:
        raise RuntimeError("sudo is required to launch Tor under its dedicated service account.")
    return [sudo, "-n", "-u", f"#{TOR_UID}", "-g", f"#{TOR_GID}", *argv]


def _verify_tor_config() -> None:
    assert TOR_BIN
    try:
        proc = subprocess.run(
            _service_command([TOR_BIN, "--verify-config", "-f", str(TORRC)]),
            stdout=subprocess.PIPE, 
            stderr=subprocess.PIPE, 
            text=True,
            timeout=25, 
            check=False,
        )
    except subprocess.TimeoutExpired:
        raise RuntimeError("Tor configuration validation timed out; the firewall has not been installed.")
    
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "Tor returned a non-zero exit status.").strip()
        if len(detail) > 1600:
            detail = detail[-1600:]
        raise RuntimeError("Tor rejected its generated configuration before the firewall was installed:\n" + detail)


def _launch_tor() -> None:
    assert TOR_BIN
    try:
        proc = subprocess.run(
            _service_command([TOR_BIN, "-f", str(TORRC)]),
            stdout=subprocess.PIPE, 
            stderr=subprocess.PIPE, 
            text=True,
            timeout=20, 
            check=False,
        )
    except subprocess.TimeoutExpired:
        raise RuntimeError("Tor did not return from startup in time. The firewall kill-switch remains active; click Disconnect to restore ordinary networking.")
    
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "").strip()
        if len(detail) > 1200:
            detail = detail[-1200:]
        raise RuntimeError("Tor failed to start. The firewall kill-switch remains active; click Disconnect to restore ordinary networking. " + (detail if detail else "Check the tor package and configuration."))
    
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        pid = _read_pid()
        if pid and _process_matches(pid):
            return
        time.sleep(0.5)
    raise RuntimeError("Tor did not create a verifiable process. The firewall kill-switch remains active; click Disconnect to restore ordinary networking.")


def connect() -> str:
    global TOR_BIN, NFT_BIN
    TOR_BIN = binary("tor")
    NFT_BIN = binary("nft")
    
    if TOR_ACCOUNT_INFO is None:
        raise RuntimeError("No dedicated Tor service account was found (expected tor, debian-tor, _tor, toranon, or torify-tor). This prototype refuses to use the shared nobody account because its other processes could bypass torification.")
    
    _sudo_authenticate()
    
    if _table_exists():
        pid = _read_pid()
        if pid and _process_matches(pid):
            try:
                if _command([binary("grep"), "-Fq", "Bootstrapped 100%", str(DATA_DIR / "tor.log")], timeout=10).returncode == 0:
                    return "Torify is already connected. The firewall kill-switch table is active."
                return "The firewall kill-switch is active, but Tor has not finished bootstrapping. Use Status or Disconnect."
            except (RuntimeError, subprocess.TimeoutExpired):
                return "The firewall kill-switch is active. Use Status or Disconnect."
        raise RuntimeError("The firewall table inet torify_app already exists but its Tor process was not verified. Use Disconnect to remove this app's stale rules, then Connect again.")
    
    _prepare_tor()
    _safe_write(RULES_FILE, _nft_rules(), 0o644)
    try:
        _checked([NFT_BIN, "-f", str(RULES_FILE)], timeout=20)
    finally:
        try:
            RULES_FILE.unlink(missing_ok=True)
        except OSError:
            pass
    
    _launch_tor()
    if not _wait_for_bootstrap():
        log = _log_tail()
        raise RuntimeError("Tor did not bootstrap. The firewall kill-switch remains active, so public traffic is blocked rather than sent directly. Click Disconnect to restore normal networking.\n\n" + (log or "No Tor log output was found."))
    
    return "Connected to Tor. Outbound TCP and DNS are routed through Tor; unsupported public UDP and other protocols are blocked."


def disconnect() -> str:
    global NFT_BIN
    NFT_BIN = NFT_BIN or shutil.which("nft")
    if not NFT_BIN:
        raise RuntimeError("nft was not found. Install nftables to remove the firewall table, or use the recovery command in README.md.")
    
    _sudo_authenticate()
    
    pid = _read_pid()
    stopped = False
    if pid and _process_matches(pid):
        try:
            _checked([binary("kill"), "-TERM", str(pid)], timeout=10)
            for _ in range(25):
                try:
                    check = _command([binary("kill"), "-0", str(pid)], timeout=5)
                    if check.returncode != 0:
                        stopped = True
                        break
                except (RuntimeError, subprocess.TimeoutExpired):
                    break
                time.sleep(0.2)
            if not stopped and _process_matches(pid):
                try:
                    _checked([binary("kill"), "-KILL", str(pid)], timeout=10)
                except RuntimeError:
                    pass
        except RuntimeError:
            pass
    
    if _table_exists():
        try:
            _checked([NFT_BIN, "delete", "table", TABLE_FAMILY, TABLE_NAME], timeout=15)
        except RuntimeError:
            pass
    
    try:
        _command([binary("rm"), "-f", str(_pidfile())], timeout=10)
    except (RuntimeError, subprocess.TimeoutExpired):
        pass
    
    try:
        RULES_FILE.unlink(missing_ok=True)
        TORRC.unlink(missing_ok=True)
    except OSError:
        pass
    
    return "Disconnected. This app's firewall table has been removed; normal network routing is restored."


def status() -> str:
    global NFT_BIN
    NFT_BIN = NFT_BIN or shutil.which("nft")
    if not NFT_BIN:
        return "Unknown: nftables command not found."
    
    if os.geteuid() != 0:
        sudo = SUDO_BIN or shutil.which("sudo")
        if not sudo:
            return "Status cannot be verified: sudo is unavailable. Run status as root."
        try:
            auth = subprocess.run([sudo, "-n", "true"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False, timeout=10)
            if auth.returncode != 0:
                return "Status cannot be verified until administrator permission is available. Run `sudo -v` in the launching terminal, then check Status again."
        except subprocess.TimeoutExpired:
            return "Status check timed out waiting for sudo authentication."
    
    if not _table_exists():
        return "Disconnected. No torify_app firewall table is installed."
    
    pid = _read_pid()
    if pid and _process_matches(pid):
        try:
            if _command([binary("grep"), "-Fq", "Bootstrapped 100%", str(DATA_DIR / "tor.log")], timeout=10).returncode == 0:
                return f"Connected (Tor PID {pid}); firewall kill-switch table is active."
            return f"Tor is running (PID {pid}) but bootstrap is incomplete; the kill-switch is active and public traffic remains blocked/routed through Tor."
        except (RuntimeError, subprocess.TimeoutExpired):
            return f"Tor is running (PID {pid}); status of bootstrap could not be determined."
    
    return "Firewall kill-switch table is active, but the Tor process was not verified. Internet access may be blocked; use Disconnect or the README recovery command."


def _print_help() -> None:
    print("""Torify Python backend / CLI (Optimized)

Usage:
  torify-cli connect | disconnect | status
  torify-cli transport list | get | set <direct|obfs4|snowflake|webtunnel|meek_lite>
  torify-cli bridge validate <bridge line>
  torify-cli bridge add <bridge line>
  torify-cli bridge list | remove <number> | clear

The C++ GUI calls this Python backend for connect/disconnect/status. Rust owns
bridge parsing, transport settings, and the Tor configuration fragment.
""")


def main() -> None:
    import argparse
    parser = argparse.ArgumentParser(description="Torify's Python Tor backend and CLI")
    parser.add_argument("args", nargs="*", help="connect, disconnect, status, transport ..., or bridge ...")
    args = parser.parse_args().args
    if not args:
        _print_help()
        return
    
    command = args[0]
    if command in {"connect", "disconnect", "status"}:
        if len(args) != 1:
            parser.error(f"{command} takes no additional arguments")
        try:
            if command == "connect":
                print(connect())
            elif command == "disconnect":
                print(disconnect())
            else:
                print(status())
        except Exception as exc:
            raise SystemExit(f"Error: {exc}")
        return
    
    if command in {"transport", "bridge"}:
        try:
            result = _run_rust(*args, timeout=60)
        except Exception as exc:
            raise SystemExit(f"Error: {exc}")
        if result.stdout:
            print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
        if result.stderr:
            print(result.stderr, end="" if result.stderr.endswith("\n") else "\n", file=__import__("sys").stderr)
        if result.returncode:
            raise SystemExit(result.returncode)
        return
    
    parser.error(f"unknown command '{command}'")


if __name__ == "__main__":
    main()
