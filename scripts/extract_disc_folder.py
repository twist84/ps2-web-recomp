"""Extract disc files; the runtime synthesizes ISO metadata in memory."""
import argparse
from pathlib import Path, PurePosixPath
import importlib.util

spec = importlib.util.spec_from_file_location("ps2_disc_tools", Path(__file__).with_name("disc.py"))
disc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(disc)
SECTOR = disc.SECTOR


def destination(root, name):
    relative = PurePosixPath(name.replace("\\", "/"))
    if relative.is_absolute() or ".." in relative.parts or not relative.parts or ":" in name:
        raise ValueError("unsafe disc filename: " + name)
    path = root.joinpath(*relative.parts)
    path.resolve().relative_to(root.resolve())
    return path


def extract_folder(image_path, output):
    image = disc.Image(image_path)
    root = Path(output)
    root.mkdir(parents=True, exist_ok=True)
    used_names = set()
    try:
        entries = list(image.files())
        for name, layer, lba, size in entries:
            key = name.replace("\\", "/").casefold()
            if key in used_names:
                raise ValueError("duplicate path across ISO layers cannot be represented without sidecar metadata: " + name)
            used_names.add(key)
            target = destination(root, name)
            if target.resolve() == Path(image_path).resolve():
                raise ValueError("extraction would overwrite the source ISO")
            if lba * SECTOR + size > image.size:
                raise ValueError("file extends beyond ISO: " + name)
            target.parent.mkdir(parents=True, exist_ok=True)
            print("extracting %s (%d bytes)" % (name, size), flush=True)
            image.file.seek(lba * SECTOR)
            with target.open("wb") as output_file:
                remaining = size
                while remaining:
                    chunk = image.file.read(min(remaining, 4 << 20))
                    if not chunk:
                        raise ValueError("truncated ISO file: " + name)
                    output_file.write(chunk)
                    remaining -= len(chunk)
        print("%d files extracted to %s; ISO layout will be generated at runtime" % (len(entries), root))
    finally:
        image.file.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", help="source ISO, needed only for extraction")
    parser.add_argument("destination", help="folder to hold the ELF and all disc files")
    args = parser.parse_args()
    try:
        extract_folder(args.image, args.destination)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
