"""Exercise the real light-list capture reader with complete and legacy masks."""
import json
import struct
import tempfile
import unittest
from pathlib import Path

from analyze_light_list import analyze_frame


class LightListCaptureReaderTests(unittest.TestCase):
    def test_reader_accepts_complete_masks_and_reports_legacy_fallback(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            cells, total = 24, 128
            ranges = bytearray(256)
            struct.pack_into("<4f", ranges, 208, 16, 16, 100, 8)
            struct.pack_into("<4I", ranges, 224, 1, 1, cells, 0)
            struct.pack_into("<4I", ranges, 240, total, 32, 32, cells)
            ranges += struct.pack("<4I", 32, 32, 1, total) * cells
            records = bytearray(total * 128)
            for index in range(total):
                struct.pack_into("<4I", records, index * 128 + 64, index + 1, 0, 1, 1)
            data = {
                "light_records": records,
                "light_ranges": ranges,
                "light_inline": struct.pack("<4I", *([0xffffffff] * 4)) * cells,
                "light_spill": bytes(4),
                "lighting_hdr": bytes(16),
            }
            resources = []
            for name, content in data.items():
                (directory / name).write_bytes(content)
                resources.append({"name": name, "file": name, "row_size": len(content),
                                  "allocation_bytes": len(content)})
            metadata = {
                "width": 16,
                "height": 16,
                "resources": resources,
                "light_list": {
                    "frame_slot_count": 3,
                    "frame_slot": 0,
                    "capacity": {"width": 16, "height": 16},
                    "index_encoding": "bitset_u32x4_complete",
                    "upload_allocation_bytes": 0,
                    "allocation_bytes": sum(len(data[name]) for name in
                                             ("light_records", "light_ranges", "light_inline",
                                              "light_spill")) * 3,
                },
            }
            capture = directory / "light-list-4.json"
            capture.write_text(json.dumps(metadata), encoding="utf-8")

            result = analyze_frame(directory, 4)
            self.assertEqual((result["overflow_cells"], result["world_query_cells"]), (cells, 0))

            # A truncated mask is rejected for the complete encoding.
            partial = struct.pack("<4I", 0xffffffff, 0xffffffff, 0, 0) * cells
            (directory / "light_inline").write_bytes(partial)
            with self.assertRaisesRegex(ValueError, "Bitset membership"):
                analyze_frame(directory, 4)

            # The old encoding remains readable and marks cells for world-query fallback.
            metadata["light_list"]["index_encoding"] = "bitset_u32x4"
            capture.write_text(json.dumps(metadata), encoding="utf-8")
            result = analyze_frame(directory, 4)
            self.assertEqual((result["overflow_cells"], result["world_query_cells"]), (cells, cells))


if __name__ == "__main__":
    unittest.main()
