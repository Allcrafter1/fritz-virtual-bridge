"""Validate metadata required by the Freetz package build."""

from pathlib import Path

PACKAGE_FILES = Path(__file__).parents[1] / "freetz" / "package" / "files"


def test_translated_freetz_cgi_is_declared() -> None:
    """Ensure Freetz resolves package labels for fixed-language builds."""
    language_manifest = (PACKAGE_FILES / ".language").read_text()
    cgi_path = "usr/lib/cgi-bin/fritzvirtual.cgi"

    assert cgi_path in language_manifest
    assert "$(lang" in (PACKAGE_FILES / "root" / cgi_path).read_text()
