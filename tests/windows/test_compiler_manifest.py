"""Read valid PE manifest encodings without changing any executable."""
from pathlib import Path
import sys
import unittest
from xml.etree import ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/windows"))
from compiler_manifest import parse_manifest


class ManifestEncodingTest(unittest.TestCase):
    def test_resource_xml_and_padding(self):
        xml = '<assembly xmlns="urn:schemas-microsoft-com:asm.v1"><description>文字 á</description></assembly>'
        for encoding in ("utf-8", "utf-8-sig", "utf-16", "utf-16-le", "utf-16-be"):
            for padded in (False, True):
                with self.subTest(encoding=encoding, padded=padded):
                    data = xml.encode(encoding)
                    if padded:
                        data += b"\0\0" if encoding.startswith("utf-16") else b"\0"
                    root = parse_manifest(data)
                    self.assertEqual(root[0].text, "文字 á")
                    self.assertEqual(root.tag, "{urn:schemas-microsoft-com:asm.v1}assembly")

    def test_malformed_xml_is_rejected(self):
        with self.assertRaises(ET.ParseError):
            parse_manifest(b"<assembly><broken></assembly>")


if __name__ == "__main__":
    unittest.main()
