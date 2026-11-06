"""Extract every disc file and preserve original file/metadata sector locations."""
import argparse
import importlib.util
from pathlib import Path, PurePosixPath
import struct

spec = importlib.util.spec_from_file_location("ps2_disc_tools", Path(__file__).with_name("disc.py"))
disc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(disc)
SECTOR = disc.SECTOR
from generate_game_manifest import generate_manifest


class LayoutImage(disc.Image):
    def __init__(self, path):
        self.metadata = {}
        super().__init__(path)

    def read(self, sector, count):
        data = super().read(sector, count)
        if len(data) != count * SECTOR:
            raise ValueError("truncated disc metadata")
        for index in range(count):
            self.metadata[sector + index] = data[index * SECTOR:(index + 1) * SECTOR]
        return data


def destination(root, name):
    relative = PurePosixPath(name.replace("\\", "/"))
    if relative.is_absolute() or ".." in relative.parts or not relative.parts or ":" in name or "\t" in name or "\n" in name:
        raise ValueError("unsafe disc filename: " + name)
    path = root.joinpath(*relative.parts)
    path.resolve().relative_to(root.resolve())
    return path


def extract_folder(image_path, output):
    image = LayoutImage(image_path)
    root = Path(output)
    root.mkdir(parents=True, exist_ok=True)
    extents = []
    used_names = set()
    try:
        entries = list(image.files())  # Captures all ISO directory sectors.
        for layer in image.layers:
            image.read(layer.base, 16)
            for sector in range(layer.base + 16, min(layer.base + 80, image.size // SECTOR)):
                descriptor = image.read(sector, 1)
                if descriptor[1:6] != b"CD001" or descriptor[0] == 255:
                    break
            pvd = image.read(layer.base + 16, 1)
            path_size = struct.unpack_from("<I", pvd, 132)[0]
            for offset, endian in [(140, "<I"), (144, "<I"), (148, ">I"), (152, ">I")]:
                lba = struct.unpack_from(endian, pvd, offset)[0]
                if lba and path_size:
                    image.read(layer.base + lba, (path_size + SECTOR - 1) // SECTOR)

        for name, layer, lba, size in entries:
            if name.split("/")[0].upper() == ".PS2-DISC":
                raise ValueError("disc uses reserved .ps2-disc name")
            storage = name
            if name.upper() in used_names:
                storage = ".ps2-disc/layers/%d/%08x/%s" % (layer.index, lba, name)
            used_names.add(name.upper())
            target = destination(root, storage)
            if target.resolve() == Path(image_path).resolve():
                raise ValueError("extraction would overwrite the source ISO")
            if (lba * SECTOR + size > image.size):
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
            if size:
                extents.append((lba, size, storage))

        metadata_dir = root / ".ps2-disc" / "metadata"
        metadata_dir.mkdir(parents=True, exist_ok=True)
        pending = sorted(image.metadata)
        at = 0
        while at < len(pending):
            start = pending[at]
            end = at + 1
            while end < len(pending) and pending[end] == pending[end - 1] + 1:
                end += 1
            storage = ".ps2-disc/metadata/%08x.bin" % start
            data = b"".join(image.metadata[sector] for sector in pending[at:end])
            destination(root, storage).write_bytes(data)
            extents.append((start, len(data), storage))
            at = end

        # ISO hard links may give several names to one physical extent.
        selected = []
        previous_end = 0
        for extent in sorted(extents, key=lambda item: (item[0], -item[1])):
            lba, size, storage = extent
            end = lba + (size + SECTOR - 1) // SECTOR
            if lba < previous_end:
                if selected and end <= previous_end and lba >= selected[-1][0]:
                    continue
                raise ValueError("partially overlapping ISO extents cannot be exported: " + storage)
            selected.append(extent)
            previous_end = end
        layout = root / ".ps2-disc" / "layout.tsv"
        temporary = layout.with_suffix(".tmp")
        with temporary.open("w", encoding="utf-8", newline="\n") as out:
            out.write("PS2_DISC_LAYOUT\t1\t%d\n" % ((image.size + SECTOR - 1) // SECTOR))
            for lba, size, storage in selected:
                out.write("%d\t%d\t%s\n" % (lba, size, storage))
        temporary.replace(layout)
        if any(name.upper() == "INFO.DAT" for name, _, _, _ in entries) and any(
            name.upper() == "GAME.DAT" for name, _, _, _ in entries
        ):
            generate_manifest(root)
        print("%d files extracted to %s; original file LBAs preserved" % (len(entries), root))
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
