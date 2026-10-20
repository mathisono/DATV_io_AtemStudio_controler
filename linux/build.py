"""Build the shared controller core with the system C++17 compiler."""
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def build():
    output = ROOT / "build" / "libdatv.so"
    output.parent.mkdir(exist_ok=True)
    subprocess.run([
        *shlex.split(os.environ.get("CXX", "g++")), "-std=c++17", "-O2",
        "-Wall", "-Wextra", "-Werror", "-pedantic", "-shared", "-fPIC",
        "-Ilib/Arbitration", "-Ilib/ProgramCoordinator", "-Ilib/AtemClient/include",
        "linux/native.cpp", "lib/Arbitration/Arbitration.cpp",
        "lib/ProgramCoordinator/ProgramCoordinator.cpp", "lib/AtemClient/src/AtemProtocol.cpp",
        "-o", str(output)], cwd=ROOT, check=True)
    return output

if __name__ == "__main__":
    print(build())
