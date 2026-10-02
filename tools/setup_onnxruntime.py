import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import venv


VERSION = "1.22.1"
HEADERS = {
    "onnxruntime_c_api.h": "d683537d0fdc29e977b5520f7f15d87a0ac212ae6d94fa9be8893a52655621ae",
    "onnxruntime_cxx_api.h": "28b21dfc371fb53d1ba67356adeb124f36643ee5ab6f1ad2e9afbf09eb4bf305",
    "onnxruntime_cxx_inline.h": "c41b9a11aac46851f68f4bd888bb2444a0e1eb6b37eadb41bf15f5154b48ea31",
    "onnxruntime_float16.h": "6c4db3d1954b266c4febe413351af86cac922e3448538e7e452e51d646e56a4a",
}


def main() -> None:
    parser = argparse.ArgumentParser(description="Prepare the ONNX Runtime C++ SDK from its Linux Python wheel.")
    parser.add_argument("--output", type=Path, default=Path("output/onnxruntime"))
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("This helper supports Linux/WSL. Otherwise supply an official SDK via ONNXRUNTIME_ROOT.")
    if not (3, 10) <= sys.version_info[:2] <= (3, 12):
        parser.error("Use Python 3.10–3.12 to install the pinned ONNX Runtime wheel.")
    root = args.output.resolve()
    environment = root / "venv"
    if not environment.exists():
        venv.EnvBuilder(with_pip=True).create(environment)
    python = environment / "bin/python"
    subprocess.run([
        str(python), "-m", "pip", "install", f"onnxruntime=={VERSION}", "numpy==1.26.4", "protobuf==4.25.8",
    ], check=True)
    library_dir = Path(subprocess.check_output([
        str(python), "-c",
        "from pathlib import Path; import onnxruntime; print(Path(onnxruntime.__file__).parent / 'capi')",
    ], text=True).strip())
    include = root / "include"
    libraries = root / "lib"
    include.mkdir(parents=True, exist_ok=True)
    libraries.mkdir(parents=True, exist_ok=True)
    for name, expected_hash in HEADERS.items():
        url = f"https://raw.githubusercontent.com/microsoft/onnxruntime/v{VERSION}/include/onnxruntime/core/session/{name}"
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != expected_hash:
            raise ValueError(f"ONNX Runtime header checksum mismatch: {name}")
        (include / name).write_bytes(data)
    with urllib.request.urlopen(
        f"https://raw.githubusercontent.com/microsoft/onnxruntime/v{VERSION}/LICENSE", timeout=60,
    ) as response:
        (root / "LICENSE").write_bytes(response.read())
    library = library_dir / f"libonnxruntime.so.{VERSION}"
    shutil.copy2(library, libraries / library.name)
    for name in ("libonnxruntime.so", "libonnxruntime.so.1"):
        link = libraries / name
        if not link.is_symlink():
            link.symlink_to(library.name)
    print(f"ONNXRUNTIME_ROOT={root}")


if __name__ == "__main__":
    main()
