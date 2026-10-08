#!/usr/bin/env python3
"""Exercise bundled startup and logging failures without retail game data.

Run against an app mounted from the DMG to catch read-only startup failures:
    python3 tools/check_macos_install.py /Volumes/.../TableTennisRecomp.app
Only processes launched here are terminated. User data is isolated in a temp dir.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def check(app, root, name, extra_args=(), broken_log_dir=False):
    data = root / name
    user = data / "tabletennis"
    user.mkdir(parents=True)
    # Confirms that config is loaded from user storage before path resolution.
    (user / "tabletennis.toml").write_text('log_level = "debug"\n')
    if broken_log_dir:
        (user / "logs").write_text("This is a file, not a log directory.\n")
    env = dict(os.environ, XDG_DATA_HOME=str(data))
    env.pop("TABLETENNIS_INSTALL_ISO", None)
    with (data / "console.txt").open("w") as console:
        process = subprocess.Popen(
            [str(app / "Contents/MacOS/tabletennis"), *extra_args],
            env=env, stdout=console, stderr=console,
        )
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                captured = (data / "console.txt").read_text()
                files = list((user / "logs").glob("*.log")) if not broken_log_dir else []
                logged = "\n".join(p.read_text() for p in files)
                output = captured + logged
                assert process.poll() is None, f"{name}: exited: {output[-2000:]}"
                if "showing the ISO installer" in output:
                    assert str(user / "game") in output, output[-2000:]
                    assert "Loaded config: tabletennis.toml" in output, output[-2000:]
                    if extra_args or broken_log_dir:
                        assert "File logging disabled:" in captured, captured[-2000:]
                    else:
                        assert files, "No default per-user log file"
                    # Allow enough time for the first wizard frame to render.
                    time.sleep(1)
                    assert process.poll() is None, f"{name}: exited after installer opened"
                    print(f"PASS {name}: installer opened, config and paths verified")
                    return
                time.sleep(0.2)
            raise AssertionError(f"{name}: installer timed out: {output[-2000:]}")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def main():
    app = Path(sys.argv[1]).resolve()
    assert app.suffix == ".app" and (app / "Contents/MacOS/tabletennis").is_file()
    with tempfile.TemporaryDirectory(prefix="tt-install-check-") as directory:
        root = Path(directory)
        check(app, root, "default")
        check(app, root, "bad-directory", broken_log_dir=True)
        blocked = root / "not-a-directory"
        blocked.write_text("Block log file creation.\n")
        check(app, root, "bad-file", [f"--log_file={blocked / 'game.log'}"])


if __name__ == "__main__":
    main()
