"""Build a diagnostic SNES cartridge and verify its recorded output, optionally against headless ares."""
import argparse
import hashlib
import json
import struct
import subprocess
import tempfile
import wave
from pathlib import Path


def cartridge(path, pal=False, modes=False):
    """Red at power-on, then changing backdrop colors and audible DSP noise with unequal stereo gains."""
    rom = bytearray(32768)
    boot = bytearray.fromhex("78 d8")  # SEI, CLD. Reset leaves A and X eight bits wide.

    def store(address, value):
        boot.extend((0xa9, value, 0x8d, address & 255, address >> 8))

    if modes:
        store(0x2100, 0x80)  # VRAM and CGRAM writes during boot require forced blank.
    store(0x2133, 0)
    store(0x2105, 0)
    store(0x212c, 0)
    store(0x2130, 0)
    store(0x2131, 0)
    store(0x2121, 0)
    store(0x2122, 31)
    store(0x2122, 0)
    if modes:
        # BG1 supplies solid blue on the subscreen. Pseudo-hires alternates these dots with the red backdrop.
        store(0x2122, 0)
        store(0x2122, 0x7c)
        store(0x2107, 0)
        store(0x210b, 1)
        store(0x2115, 0x80)
        store(0x2116, 0)
        store(0x2117, 0)
        # Clear the 32x32 tilemap even on a PPU with randomized power-on VRAM.
        boot.extend(bytes.fromhex("a0 04 a2 00 9c 18 21 9c 19 21 ca d0 f7 88 d0 f2"))
        store(0x2117, 0x10)
        boot.extend(bytes.fromhex("a2 08"))
        tile = len(boot)
        store(0x2118, 255)
        store(0x2119, 0)
        boot.extend((0xca, 0xd0, (tile - len(boot) - 3) & 255))
        store(0x212d, 1)
    store(0x2100, 15)

    spc = bytearray()
    def direct(address, value):
        spc.extend((0x8f, value, address))  # MOV dp,#imm
    for address, value in enumerate([0x50, 0, 0x50, 0], 0x40):
        direct(address, value)
    for address in range(0x50, 0x59):
        direct(address, 3 if address == 0x50 else 0)  # A looping BRR block keeps voice zero active.
    for register, value in [(0x6c, 0x3f), (0x0c, 127), (0x1c, 127), (0x5d, 0),
                            (0x04, 16), (0x05, 0), (0x07, 127), (0x00, 100), (0x01, 55),
                            (0x3d, 1), (0x5c, 0), (0x4c, 1)]:
        direct(0xf2, register)
        direct(0xf3, value)
    spc.extend((0x2f, 0xfe))  # BRA $

    # Follow the SPC IPL byte-transfer handshake, then execute the payload at $0200.
    boot.extend(bytes.fromhex("ad 40 21 c9 aa d0 f9"))
    store(0x2142, 0)
    store(0x2143, 2)
    store(0x2141, 1)
    store(0x2140, 0xcc)
    boot.extend(bytes.fromhex("cd 40 21 d0 fb a2 00"))
    loop = len(boot)
    boot.extend(bytes.fromhex("bd 00 84 8d 41 21 8e 40 21 ec 40 21 d0 fb e8 e0"))
    boot.extend((len(spc), 0xd0, (loop - len(boot) - 3) & 255))
    store(0x2141, 0)
    store(0x2140, len(spc) + 1)

    boot.extend(bytes.fromhex("a2 00"))
    loop = len(boot)
    # Wait for vblank to end and start, then write a unique 5-bit red level for this frame.
    boot.extend(bytes.fromhex("ad 12 42 30 fb ad 12 42 10 fb"))
    if modes:
        # Changing overscan at line 225 can end vblank immediately. Latch the V counter until both modes are blank.
        boot.extend(bytes.fromhex("ad 3f 21 ad 37 21 ad 3d 21 c9 f2 90 f3"))
    boot.extend(bytes.fromhex("9c 21 21 e8 8a 29 1f 8d 22 21 9c 22 21"))
    if modes:
        # SETINI comes from a field-indexed table. The second hires field switches back to low resolution mid-picture.
        boot.extend(bytes.fromhex("8a 29 07 a8 b9 00 85 8d 33 21 e0 02 d0 12"))
        boot.extend(bytes.fromhex("ad 12 42 30 fb a0 08 a9 ff 3a d0 fd 88 d0 f8 9c 33 21"))
        rom[0x500:0x508] = bytes([0, 8, 8, 13, 13, 13, 0, 0])
        # After the mid-field delay the vblank flag is already clear, so the common wait loop is still valid.
    boot.extend((0x80, (loop - len(boot) - 2) & 255))
    assert len(boot) < 1024 and len(spc) < 256
    rom[:len(boot)] = boot
    rom[0x400:0x400+len(spc)] = spc
    rom[0x7fc0:0x7fd5] = b"NATIVE MOVIE TEST".ljust(21)
    rom[0x7fd5:0x7fdc] = bytes([0x20, 0, 5, 0, 2 if pal else 1, 0x33, 0])
    rom[0x7ffc:0x7ffe] = struct.pack("<H", 0x8000)
    checksum = (sum(rom) + 510) & 65535
    struct.pack_into("<HH", rom, 0x7fdc, checksum ^ 65535, checksum)
    path.write_bytes(rom)


def run(*args):
    return subprocess.check_output(args)


def emulate(headless, rom, settings, accurate, frames, *output):
    run(headless, "--system", "sfc", "--settings-file", settings,
        "--setting", f"Video/PixelAccuracy={str(accurate).lower()}",
        "--run-frames", str(frames), *output, rom)


def verify_pcm(pcm, headless, rom, tmp, accurate, count):
    # The threaded headless stop can enter the next field. Compare exactly this recording's interval.
    emulate(headless, rom, tmp / "settings.bml", accurate, count + 2, "--dump-audio", tmp / "boot.wav")
    with wave.open(str(tmp / "boot.wav")) as wav:
        reference = wav.readframes(len(pcm) // 4)
    assert any(pcm) and pcm == reference, "PCM differs from independently mixed boot audio"


def verify_modes(movie, headless, rom, accurate):
    """Check the first mode-change cycle and all PCM across consecutive parts."""
    parts = [movie]
    while True:
        sibling = movie.with_name(f"{movie.stem}-{len(parts) + 1}.mkv")
        if not sibling.exists():
            break
        parts.append(sibling)
    assert len(parts) >= 4, "capture at least eight fields"
    pcm = bytearray()
    count = 0
    for index, part in enumerate(parts):
        video, audio = json.loads(run("ffprobe", "-v", "error", "-count_frames", "-show_streams",
                                      "-of", "json", part))["streams"]
        assert video["codec_name"] == "ffv1"
        assert (audio["codec_name"], audio["channels"], int(audio["sample_rate"])) == ("pcm_s16le", 2, 48000)
        frames = int(video["nb_read_frames"])
        count += frames
        pcm.extend(run("ffmpeg", "-v", "error", "-i", part, "-map", "0:a", "-f", "s16le", "-"))
        if index > 3:
            continue
        width, height, needed = [(256, 224, 1), (512, 224, 2), (512, 478, 3), (256, 224, 2)][index]
        assert (video["width"], video["height"]) == (width, height)
        if index < 3:
            assert frames == needed
        else:
            assert frames >= needed
        raw = run("ffmpeg", "-v", "error", "-i", part, "-map", "0:v", "-frames:v", str(needed),
                  "-pix_fmt", "rgb24", "-fps_mode", "passthrough", "-f", "rawvideo", "-")

        def dots(frame, row):
            start = ((height * frame + row) * width + 100) * 3
            return raw[start:start + 6]

        def red(level):
            return bytes([(level << 3) | (level >> 2), 0, 0])

        blue = bytes([0, 0, 255])
        if index == 1:
            assert dots(0, 100) == blue + red(1)
            assert dots(1, 20) == blue + red(2) and dots(1, 150) == red(2) * 2
        if index == 2:
            # Seed both parities on entry, then update alternating rows down to the final overscan line.
            for frame, (even, odd) in enumerate([(3, 3), (4, 3), (4, 5)]):
                for y in range(0, height, 2):
                    assert dots(frame, y) == blue + red(even)
                    assert dots(frame, y + 1) == blue + red(odd)
        if index == 3:
            assert dots(0, 100) == red(6) * 2 and dots(1, 100) == red(7) * 2
    assert any(pcm), "diagnostic cartridge produced no audible samples"
    if headless:
        with tempfile.TemporaryDirectory() as tmp:
            verify_pcm(pcm, headless, rom, Path(tmp), accurate, count)
    print(f"{len(parts)} parts, {count} fields, {len(pcm)//4} stereo samples")


def verify(movie, headless, rom, pal, accurate):
    info = json.loads(run("ffprobe", "-v", "error", "-show_streams", "-of", "json", movie))["streams"]
    video, audio = info
    assert (video["codec_name"], video["pix_fmt"], video["width"], video["height"]) == ("ffv1", "bgr0", 256, 224)
    assert (audio["codec_name"], audio["channels"], int(audio["sample_rate"])) == ("pcm_s16le", 2, 48000)
    packets = json.loads(run("ffprobe", "-v", "error", "-select_streams", "v", "-show_packets",
                             "-show_entries", "packet=pts_time", "-of", "json", movie))["packets"]
    ticks = 0
    frequency = 21281370 if pal else 236250000 / 11
    for index, packet in enumerate(packets):
        assert abs(float(packet["pts_time"]) - ticks / frequency) < 0.001, (index, packet, ticks)
        if index == 0:
            ticks += 240 * 1364
        else:
            ticks += (312 if pal else 262) * 1364 - (4 if not pal and index % 2 == 0 else 0)
    count = len(packets)
    assert count > 5
    # After its saturated boot frame the ROM increments the native red level every vblank.
    # Hash complete RGB pictures so a skipped, duplicated or cropped scanline fails the check.
    levels = []
    for level in range(32):
        red = (level << 3) | (level >> 2)
        levels.append(hashlib.md5(bytes([red, 0, 0]) * (256 * 224)).hexdigest())
    hashes = run("ffmpeg", "-v", "error", "-i", movie, "-map", "0:v", "-pix_fmt", "rgb24",
                 "-fps_mode", "passthrough", "-f", "framemd5", "-").decode().splitlines()
    pictures = [line.split(",")[-1].strip() for line in hashes if not line.startswith("#")]
    assert len(pictures) == count
    assert pictures[0] == levels[31], "expected the saturated red power-on field"
    for index in range(1, count):
        assert pictures[index] == levels[index % 32], ("native RGB", index)
    pcm = run("ffmpeg", "-v", "error", "-i", movie, "-map", "0:a", "-f", "s16le", "-")
    assert any(pcm), "diagnostic cartridge produced no audible samples"
    error = abs(len(pcm) / (4 * 48000) - ticks / frequency)
    assert error < 0.001, ("audio endpoint", error)
    # An independent boot additionally checks every PCM sample and the first displayed picture.
    if headless:
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            verify_pcm(pcm, headless, rom, tmp, accurate, count)
            # The default headless PNG includes overscan borders. Compare only the native active picture.
            emulate(headless, rom, tmp / "settings.bml", accurate, 1, "--save-last-frame", tmp / "first.png")
            crop = "crop=256:224:13:9" if not pal else "crop=256:224:11:29"
            first = run("ffmpeg", "-v", "error", "-i", tmp / "first.png", "-vf", crop, "-pix_fmt", "rgb24", "-f", "rawvideo", "-")
            decoded = run("ffmpeg", "-v", "error", "-i", movie, "-map", "0:v", "-frames:v", "1",
                          "-pix_fmt", "rgb24", "-f", "rawvideo", "-")
            assert first == decoded, "first native picture mismatch"
    print(f"{count} native frames, {len(pcm)//4} stereo samples, endpoint error {error*1000:.3f} ms")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    parser.add_argument("--modes", action="store_true", help="exercise pseudo-hires, mixed scanlines, overscan and interlace")
    parser.add_argument("--pal", action="store_true")
    parser.add_argument("--accurate", action="store_true")
    parser.add_argument("--movie", type=Path)
    parser.add_argument("--headless", type=Path, help="compare against an independent headless boot")
    args = parser.parse_args()
    if args.movie:
        if args.modes:
            verify_modes(args.movie, args.headless, args.rom, args.accurate)
        else:
            verify(args.movie, args.headless, args.rom, args.pal, args.accurate)
    elif not args.headless and not args.rom.exists():
        cartridge(args.rom, args.pal, args.modes)
    else:
        parser.error("use --movie to verify a recording, or supply a fresh ROM filename to generate one")
