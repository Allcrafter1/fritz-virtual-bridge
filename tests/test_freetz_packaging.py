"""Validate metadata required by the Freetz package build."""

from pathlib import Path

PACKAGE_FILES = Path(__file__).parents[1] / "freetz" / "package" / "files"


def test_translated_freetz_cgi_is_declared() -> None:
    """Ensure Freetz resolves package labels for fixed-language builds."""
    language_manifest = (PACKAGE_FILES / ".language").read_text()
    cgi_path = "usr/lib/cgi-bin/fritzvirtual.cgi"

    assert cgi_path in language_manifest
    assert "$(lang" in (PACKAGE_FILES / "root" / cgi_path).read_text()


def test_provider_watchdog_is_packaged() -> None:
    """Keep the local AHA control channel self-healing after deployment."""
    init_script = (
        PACKAGE_FILES / "root" / "etc" / "init.d" / "rc.fritzvirtual"
    ).read_text()

    assert "provider health check failed three times" in init_script
    assert '"$0" watchdog' in init_script
    assert "stop_watchdog || return 1" in init_script
    assert "stop_bridge || return 1" in init_script
    assert "fritzvirtual-mqtt-bridge" in init_script
    assert "MQTT bridge exited during startup; restoring stock aha" in init_script
