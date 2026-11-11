"""Adapt only the pinned upstream v0.2.0 AIDL encoder to cros input handles.

This is an alternative to rebuilding the source component. It does not install
anything, change the allocator, or patch a running process.
"""

import argparse
import hashlib
from pathlib import Path

ORIGINAL_SHA256 = '8dd025fdbfc281341f92e2d858ce661eb11e1f454d255274568da064fdcdad9a'
PATCHES = ((0xEC40, '4c8b7c8814', '4c8b7c2054'),
           (0xEC45, '448b6c8824', '448b6c2020'))


def patch(data):
    if hashlib.sha256(data).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Unknown binary: expected the original v0.2.0 x86_64 AIDL service')
    output = bytearray(data)
    for offset, before_hex, after_hex in PATCHES:
        before, after = bytes.fromhex(before_hex), bytes.fromhex(after_hex)
        if data[offset:offset + len(before)] != before:
            raise ValueError('Unexpected original handle-load instructions')
        output[offset:offset + len(before)] = after
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = patch(args.input.read_bytes())
    with args.output.open('xb') as output:
        output.write(result)
    args.output.chmod(0o755)
    print(hashlib.sha256(result).hexdigest(), args.output)


if __name__ == '__main__':
    main()
