#!/usr/bin/env python3
import glob
import os
import sys

# ~100 KB per-attachment context extraction cap; 10 max attachments per prompt (1 index + 9 chunks)
MAX_CHUNK_SIZE = 90 * 1024  # 90 KB
MAX_CHUNKS = 9
EXCLUDED_EXTS = (".o", ".d", ".elf", ".bin", ".cpio", ".gz", ".tar", ".xz", ".bz2")


def is_binary(file_path: str) -> bool:
    try:
        with open(file_path, "tr", encoding="utf-8") as check_file:
            check_file.read(1024)
            return False
    except UnicodeDecodeError:
        return True


def gather_files(paths: list[str]) -> list[str]:
    gathered = []
    for path in paths:
        if os.path.isfile(path):
            gathered.append(path)
        elif os.path.isdir(path):
            for root, dirs, files in os.walk(path):
                dirs.sort()
                files.sort()
                for name in files:
                    full_path = os.path.join(root, name)
                    if not full_path.endswith(EXCLUDED_EXTS):
                        gathered.append(full_path)
    return [f for f in gathered if os.path.getsize(f) > 0 and not is_binary(f)]


def main() -> None:
    args = sys.argv[1:] or ["arch", "kernel", "user", "tests", "Makefile"]
    valid_files = gather_files(args)

    # Remove stale chunks from previous runs
    for old_chunk in glob.glob("dump-for-ai-*.txt"):
        os.remove(old_chunk)

    chunk_idx = 1
    current_chunk_size = 0
    index_rows: list[tuple[str, int, str]] = []
    current_file = open(f"dump-for-ai-{chunk_idx}.txt", "w", encoding="utf-8")

    for path in valid_files:
        with open(path, "r", encoding="utf-8", errors="ignore") as infile:
            content = infile.read()

        header = f"\n// {'=' * 74}\n// File: {path}\n// {'=' * 74}\n\n"
        entry = header + content
        entry_size = len(entry.encode("utf-8"))

        if current_chunk_size + entry_size > MAX_CHUNK_SIZE and current_chunk_size > 0:
            current_file.close()
            chunk_idx += 1
            current_file = open(f"dump-for-ai-{chunk_idx}.txt", "w", encoding="utf-8")
            current_chunk_size = 0

        chunk_name = f"dump-for-ai-{chunk_idx}.txt"
        current_file.write(entry)
        current_chunk_size += entry_size
        index_rows.append((chunk_name, os.path.getsize(path), path))

    if not current_file.closed:
        current_file.close()

    total_bytes = sum(size for _, size, _ in index_rows)
    with open("dump-index.txt", "w", encoding="utf-8") as index_file:
        index_file.write(
            f"Total files: {len(valid_files)} | Total source size: {total_bytes} bytes "
            f"| Chunks: {chunk_idx} (max {MAX_CHUNK_SIZE // 1024} KB each)\n\n"
        )
        index_file.write(f"{'Chunk':<20} | {'Bytes':>8} | File Path\n")
        index_file.write("-" * 80 + "\n")
        for chunk_name, size, path in index_rows:
            index_file.write(f"{chunk_name:<20} | {size:>8} | {path}\n")

    print(
        f"[AI-DUMP] Generated dump-index.txt and {chunk_idx} chunk(s) "
        f"({total_bytes / 1024:.1f} KB total)."
    )
    if chunk_idx > MAX_CHUNKS:
        print(
            f"[AI-DUMP] Warning: Generated {chunk_idx} chunks, exceeding the "
            f"{MAX_CHUNKS}-chunk single-prompt limit. Upload them across multiple prompts."
        )


if __name__ == "__main__":
    main()