import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import gen_catalog as catalog


class CatalogNames(unittest.TestCase):
    def test_one_name_at_two_offsets_is_rejected(self):
        rows = [(0xA0, 10, 213, 1, -1, "O/U MPU ID"), (0xA0, 11, 213, 1, -1, "O/U MPU ID")]
        self.assertIn("O/U MPU ID", catalog.duplicate_locations(rows))

    def test_same_byte_different_bit_conversions_are_different_values(self):
        rows = [(0x10, 1, 304, 1, -1, "Status"), (0x10, 1, 305, 1, -1, "Status")]
        self.assertIn("Status", catalog.duplicate_locations(rows))

    def test_repeated_identical_entry_and_placeholders_are_allowed(self):
        row = (0x10, 1, 304, 1, -1, "Status")
        self.assertFalse(catalog.duplicate_locations([row, row, (1, 0, 211, 1, -1, "Not in use"), (2, 0, 211, 1, -1, "Not in use")]))

    def test_all_english_definitions_have_unique_effective_names(self):
        files = [p.name for p in pathlib.Path(catalog.DEF_DIR).glob("*.h") if p.name not in catalog.EXCLUDED]
        doubled = catalog.doubled_names(files)
        for filename in files:
            rows = []
            for reg, offset, conv, size, dtype, label, _ in catalog.parse(pathlib.Path(catalog.DEF_DIR) / filename):
                if catalog.FAMILIES[filename] not in "SR":
                    label = catalog.unique_name(reg, label, label, doubled)
                rows.append((reg, offset, conv, size, dtype, label))
            with self.subTest(filename=filename):
                self.assertFalse(catalog.duplicate_locations(rows))


if __name__ == "__main__":
    unittest.main()
