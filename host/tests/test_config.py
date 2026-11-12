# SPDX-License-Identifier: Apache-2.0
"""Tests for the one configuration file of the four tools (gap 99).

Every level of the precedence chain is exercised on its own and against the
others: built-in default, configuration file (with `[common]` and a section
per tool), environment, command line. The tests write their own INI file and
point `PASSERELLE_CONFIG` at it, so they never read what the machine running
them happens to have configured.
"""

from __future__ import annotations

import argparse
import json
import pathlib

import pytest

from bench.cli import main as pbench_main
from fsproof.cli import main as fsproof_main
from pgwutil import ValidationError
from pgwutil import config
from qshare.cli import main as qshare_main
from qshare.options import DEFAULT_TAG, RECOMMENDED_SECURITY
from qshare.qrun import main as qrun_main

INI = """\
[common]
tag = projet
timeout = 45

[qshare]
arch = aarch64

[qrun]
share = /srv/share

[fsproof]
workers = 8

[pbench]
scale = 0.25
"""


@pytest.fixture()
def ini_path(tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch) -> pathlib.Path:
    """A configuration file with one value per section, made the active one."""
    path = tmp_path / "passerelle.ini"
    path.write_text(INI, encoding="utf-8")
    monkeypatch.setenv(config.CONFIG_PATH_ENV, str(path))
    return path


def write_ini(tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch, text: str) -> pathlib.Path:
    """Write *text* as the active configuration file and return its path."""
    path = tmp_path / "passerelle.ini"
    path.write_text(text, encoding="utf-8")
    monkeypatch.setenv(config.CONFIG_PATH_ENV, str(path))
    return path


def test_default_path_follows_xdg() -> None:
    path = config.default_path({"XDG_CONFIG_HOME": "/tmp/xdg", "HOME": "/tmp/home"})
    assert path == pathlib.Path("/tmp/xdg/passerelle/passerelle.ini")


def test_default_path_falls_back_to_home() -> None:
    path = config.default_path({"HOME": "/tmp/home"})
    assert path == pathlib.Path("/tmp/home/.config/passerelle/passerelle.ini")


def test_explicit_config_path_wins() -> None:
    env = {"XDG_CONFIG_HOME": "/tmp/xdg", config.CONFIG_PATH_ENV: "/tmp/elsewhere.ini"}
    assert config.default_path(env) == pathlib.Path("/tmp/elsewhere.ini")


def test_every_tool_reads_the_common_section(ini_path: pathlib.Path) -> None:
    """One value set once applies to the four tools (the point of gap 99)."""
    for tool in config.TOOLS:
        cfg = config.load(tool)
        assert cfg.get("tag") == "projet", tool
        assert cfg.source("tag") == config.FROM_FILE, tool
        assert cfg.typed("timeout") == 45.0, tool


def test_tool_section_only_applies_to_its_tool(ini_path: pathlib.Path) -> None:
    assert config.load("qshare").get("arch") == "aarch64"
    assert not config.load("qrun").has("arch")
    assert config.load("qrun").get("share") == "/srv/share"
    assert config.load("fsproof").typed("workers") == 8
    assert config.load("pbench").typed("scale") == 0.25


def test_tool_section_beats_common_section(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_ini(tmp_path, monkeypatch, "[common]\ntag = common\n[qshare]\ntag = tool\n")
    cfg = config.load("qshare")
    assert cfg.get("tag") == "tool"
    assert cfg.provenance("tag") == "file: qshare"
    assert config.load("qrun").get("tag") == "common"


def test_environment_beats_the_file(
    ini_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv(config.env_var("common", "tag"), "envtag")
    cfg = config.load("qshare")
    assert cfg.get("tag") == "envtag"
    assert cfg.source("tag") == config.FROM_ENV
    assert cfg.provenance("tag") == f"env: {config.env_var('common', 'tag')}"


def test_tool_environment_beats_common_environment(
    ini_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv(config.env_var("common", "tag"), "common-env")
    monkeypatch.setenv(config.env_var("qshare", "tag"), "tool-env")
    assert config.load("qshare").get("tag") == "tool-env"
    assert config.load("qrun").get("tag") == "common-env"


def test_built_in_defaults_when_nothing_is_configured() -> None:
    cfg = config.load("qshare")
    assert cfg.get("tag") == DEFAULT_TAG
    assert cfg.get("security") == RECOMMENDED_SECURITY
    assert cfg.source("tag") == config.FROM_DEFAULT
    assert not cfg.path.is_file()


def test_command_line_beats_environment_file_and_default(
    ini_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv(config.env_var("common", "tag"), "envtag")
    namespace = argparse.Namespace(tag="clitag", arch=None)
    cfg = config.apply(namespace, config.load("qshare"))
    assert namespace.tag == "clitag"
    assert cfg.get("tag") == "clitag"
    assert cfg.source("tag") == config.FROM_CLI
    assert namespace.arch == "aarch64"  # untouched: the file answers


def test_unset_options_are_filled_from_the_configuration(ini_path: pathlib.Path) -> None:
    namespace = argparse.Namespace(tag=None, timeout=None, arch=None, scale=None)
    config.apply(namespace, config.load("pbench"))
    assert namespace.tag == "projet"
    assert namespace.timeout == 45.0
    assert namespace.scale == 0.25
    assert namespace.arch is None  # pbench has no 'arch' setting: left alone


def test_typed_values_and_a_readable_error(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_ini(tmp_path, monkeypatch, "[fsproof]\ncount = not-a-number\n")
    cfg = config.load("fsproof")
    assert cfg.get("count") == "not-a-number"
    with pytest.raises(ValidationError, match="must be int"):
        cfg.typed("count")


def test_boolean_setting_casts_the_usual_words() -> None:
    setting = config.Setting("flag", "false", kind="bool")
    assert setting.cast("yes") is True
    assert setting.cast("0") is False
    assert setting.cast("OFF") is False


def test_unknown_key_is_reported_not_fatal(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_ini(tmp_path, monkeypatch, "[qshare]\ntag = projet\ncolour = blue\n")
    cfg = config.load("qshare")
    assert cfg.get("tag") == "projet"
    assert cfg.unknown == ("colour",)
    assert "colour" in config.render(cfg)


def test_malformed_file_is_a_readable_error(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_ini(tmp_path, monkeypatch, "this is not an INI file\n")
    with pytest.raises(ValidationError, match="cannot read"):
        config.load("qshare")


def test_render_text_shows_value_and_source(ini_path: pathlib.Path) -> None:
    text = config.render(config.load("qshare"))
    assert "passerelle configuration for qshare" in text
    assert str(ini_path) in text
    assert "projet" in text and "file: common" in text
    assert "command line > env" in text


def test_render_json_is_machine_readable(ini_path: pathlib.Path) -> None:
    payload = json.loads(config.render(config.load("pbench"), "json"))
    assert payload["tool"] == "pbench"
    assert payload["file_exists"] is True
    assert payload["values"]["scale"]["value"] == "0.25"
    assert payload["precedence"][0] == config.FROM_CLI


def test_help_text_names_the_configuration_key() -> None:
    line = config.help_text("tag", "mount tag")
    assert line.startswith("mount tag")
    assert "'tag'" in line and DEFAULT_TAG in line


@pytest.mark.parametrize(
    ("run", "argv"),
    [
        (qshare_main, ["--show-config"]),
        (qrun_main, ["--show-config"]),
        (fsproof_main, ["--show-config"]),
        (pbench_main, ["--show-config"]),
    ],
)
def test_show_config_on_the_four_tools(
    run, argv: list[str], ini_path: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    assert run(argv) == 0
    out = capsys.readouterr().out
    assert "passerelle configuration for" in out
    assert "projet" in out  # the [common] value of the file


@pytest.mark.parametrize(
    ("run", "argv"),
    [
        (qshare_main, ["--show-config", "json"]),
        (qrun_main, ["--show-config", "json"]),
        (fsproof_main, ["--show-config", "json"]),
        (pbench_main, ["--show-config", "json"]),
    ],
)
def test_show_config_json_on_the_four_tools(
    run, argv: list[str], ini_path: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    assert run(argv) == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["values"]["tag"]["value"] == "projet"


def test_qshare_emit_uses_the_configured_tag(
    ini_path: pathlib.Path, share: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    assert qshare_main([str(share), "--emit"]) == 0
    assert "mount_tag=projet" in capsys.readouterr().out


def test_qshare_command_line_still_wins(
    ini_path: pathlib.Path, share: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    assert qshare_main([str(share), "--emit", "--tag", "clitag"]) == 0
    assert "mount_tag=clitag" in capsys.readouterr().out


def test_qrun_uses_the_configured_share(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """`[qrun] share` is the directory qrun runs the program from."""
    shared = tmp_path / "shared"
    shared.mkdir()
    program = shared / "hello.sh"
    program.write_text("#!/bin/sh\npwd\n", encoding="utf-8")
    program.chmod(0o755)
    write_ini(tmp_path, monkeypatch, f"[qrun]\nshare = {shared}\n")
    assert qrun_main(["--local", "hello.sh"]) == 0
    assert capsys.readouterr().out.strip() == str(shared.resolve())


def test_library_defaults_come_from_the_configuration_table() -> None:
    """One definition of a default: the library reads the config table."""
    assert DEFAULT_TAG == config.DEFAULTS["tag"]
    assert RECOMMENDED_SECURITY == config.DEFAULTS["security"]
    assert config.DEFAULTS["proto"] in ("9p", "virtiofs")
