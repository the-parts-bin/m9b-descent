#!/usr/bin/env python3
"""Converts a Descent HMP song (HMI's MIDI variant) to a standard MIDI file.

HMP ("HMIMIDIP", the old format Descent 1 uses; "HMIMIDIP013195", the new
one, also read): the track count at 0x30, the tempo in ticks per second at
0x38, the tracks from 0x308 (0x388 in the new format), each a 12-byte
header (track number, chunk length including the header, unused) and the
events. Events are MIDI's, except for the delta times: 7 bits per byte,
least significant first, the last byte with bit 7 set.

The MIDI file is format 1 with one track per HMP track (track 0 holds only
the song's length) and a tempo of 1,000,000 us per quarter note with the
HMP tempo as the division, so a tick lasts 1/tempo s as in the HMP.

Usage: hmp2mid.py IN.HMP OUT.MID"""
import struct
import sys


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def convert_track(data):
    out = bytearray()
    i, status = 0, 0
    while i < len(data):
        # HMI delta time
        t, shift = 0, 0
        while i < len(data) and not data[i] & 0x80:
            t |= data[i] << shift
            shift += 7
            i += 1
        if i >= len(data):
            break
        t |= (data[i] & 0x7F) << shift
        i += 1
        out += vlq(t)
        b = data[i]
        if b == 0xFF:                       # meta: type, length (one byte), data
            typ, n = data[i + 1], data[i + 2]
            out += data[i:i + 3 + n]
            i += 3 + n
            if typ == 0x2F:
                return out
            continue
        if b in (0xF0, 0xF7):               # sysex: length as a MIDI VLQ
            j, n = i + 1, 0
            while data[j] & 0x80:
                n = (n << 7) | (data[j] & 0x7F)
                j += 1
            n = (n << 7) | data[j]
            j += 1
            out += data[i:j + n]
            i = j + n
            continue
        if b & 0x80:
            status = b
            i += 1
        n = 1 if status & 0xF0 in (0xC0, 0xD0) else 2
        out.append(status)
        out += data[i:i + n]
        i += n
    out += b"\x00\xff\x2f\x00"              # no end-of-track: add one
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    d = open(sys.argv[1], "rb").read()
    if not d.startswith(b"HMIMIDIP"):
        sys.exit(f"{sys.argv[1]}: not an HMP file")
    pos = 0x388 if d.startswith(b"HMIMIDIP013195") else 0x308
    (ntracks,) = struct.unpack_from("<I", d, 0x30)
    (tempo,) = struct.unpack_from("<I", d, 0x38)
    tracks = []
    for _ in range(ntracks):
        _num, length, _ = struct.unpack_from("<III", d, pos)
        tracks.append(convert_track(d[pos + 12:pos + length]))
        pos += length
    out = bytearray(b"MThd" + struct.pack(">IHHH", 6, 1, ntracks + 1, tempo))
    tempo_track = b"\x00\xff\x51\x03" + (1000000).to_bytes(3, "big") + b"\x00\xff\x2f\x00"
    for t in [tempo_track] + tracks:
        out += b"MTrk" + struct.pack(">I", len(t)) + t
    open(sys.argv[2], "wb").write(out)


main()
