import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("torify_backend", ROOT / "python" / "torify.py")
backend = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(backend)


class BackendTransportIntegrationTests(unittest.TestCase):
    def make_helper(self, directory: Path, exit_code: int, output: str, error: str = "") -> Path:
        helper = directory / "mock-torify-rs"
        helper.write_text(
            "#!/usr/bin/env python3\n"
            "import sys\n"
            f"sys.stdout.write({output!r})\n"
            f"sys.stderr.write({error!r})\n"
            f"raise SystemExit({exit_code})\n",
            encoding="utf-8",
        )
        helper.chmod(0o700)
        return helper

    def test_rust_config_fragment_is_included(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            helper = self.make_helper(base, 0, "UseBridges 1\\nBridge obfs4 198.51.100.5:443 cert=abc iat-mode=0\\n")
            with patch.dict(os.environ, {"TORIFY_CONFIG_DIR": str(base / "config"), "TORIFY_RUST_BIN": str(helper)}):
                config = backend._tor_config()
            self.assertIn("UseBridges 1", config)
            self.assertIn("Bridge obfs4 198.51.100.5:443", config)
            self.assertIn("TransPort 127.0.0.1:19540", config)

    def test_rust_configuration_errors_are_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            helper = self.make_helper(base, 2, "", "Error: obfs4 bridge is missing")
            with patch.dict(os.environ, {"TORIFY_CONFIG_DIR": str(base / "config"), "TORIFY_RUST_BIN": str(helper)}):
                with self.assertRaisesRegex(RuntimeError, "obfs4 bridge is missing"):
                    backend._transport_config()

    def test_direct_mode_can_fall_back_without_rust_when_no_settings_exist(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            with patch.dict(os.environ, {
                "TORIFY_CONFIG_DIR": str(base / "config"),
                "TORIFY_RUST_BIN": str(base / "missing-helper"),
                "PATH": str(base / "empty-path"),
            }, clear=False):
                self.assertEqual(backend._transport_config(), "")

    def test_dns_is_redirected_before_private_network_exemptions(self):
        rules = backend._nft_rules()
        dns_redirect = rules.index("udp dport 53 redirect to :")
        private_bypass = rules.index("ip daddr { 0.0.0.0/8")
        self.assertLess(dns_redirect, private_bypass)

    def test_config_dir_must_be_absolute(self):
        with patch.dict(os.environ, {"TORIFY_CONFIG_DIR": "relative/path"}, clear=False):
            with self.assertRaisesRegex(RuntimeError, "must be an absolute path"):
                backend._settings_dir()


if __name__ == "__main__":
    unittest.main()
