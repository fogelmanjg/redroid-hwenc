import unittest
from unittest.mock import patch as mock_patch

import patch_vendor


class PatchTests(unittest.TestCase):
    def test_refuses_unknown_binary(self):
        with self.assertRaisesRegex(ValueError, 'Unknown binary'):
            patch_vendor.patch(b'not the release payload')

    def test_changes_only_the_four_layout_bytes(self):
        fixture = bytearray(0xED00)
        for offset, before, _ in patch_vendor.PATCHES:
            fixture[offset:offset + 5] = bytes.fromhex(before)
        # Synthetic instruction fixture: bypass only its whole-file digest.
        with mock_patch('patch_vendor.hashlib.sha256') as digest:
            digest.return_value.hexdigest.return_value = patch_vendor.ORIGINAL_SHA256
            result = patch_vendor.patch(bytes(fixture))
        changed = [i for i, (old, new) in enumerate(zip(fixture, result)) if old != new]
        self.assertEqual(changed, [0xEC43, 0xEC44, 0xEC48, 0xEC49])
        self.assertEqual(len(result), len(fixture))

    def test_instruction_guard_is_independent_of_hash_guard(self):
        with mock_patch('patch_vendor.hashlib.sha256') as digest:
            digest.return_value.hexdigest.return_value = patch_vendor.ORIGINAL_SHA256
            with self.assertRaisesRegex(ValueError, 'Unexpected original'):
                patch_vendor.patch(bytes(0xED00))


if __name__ == '__main__':
    unittest.main()
