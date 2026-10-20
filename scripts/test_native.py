"""Compile and run hardware-independent control and UDP session tests."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUITES = {
    "arbitration": [
        "-Ilib/Arbitration",
        "lib/Arbitration/Arbitration.cpp",
        "test/native/arbitration_test.cpp",
    ],
    "atem-session": [
        "-Ilib/AtemClient/include",
        "lib/AtemClient/src/AtemProtocol.cpp",
        "lib/AtemClient/tests/session_test.cpp",
    ],
    "program-coordinator": [
        "-Ilib/Arbitration",
        "-Ilib/ProgramCoordinator",
        "lib/ProgramCoordinator/ProgramCoordinator.cpp",
        "test/native/program_coordinator_test.cpp",
    ],
}

with tempfile.TemporaryDirectory(prefix="datv-native-") as directory:
    for name, sources in SUITES.items():
        executable = Path(directory) / name
        command = [*shlex.split(os.environ.get("CXX", "g++")), "-std=c++17",
                   "-Wall", "-Wextra", "-Werror", "-pedantic", *sources,
                   "-o", str(executable)]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)
