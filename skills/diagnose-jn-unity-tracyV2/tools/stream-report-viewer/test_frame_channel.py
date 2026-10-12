import json
from pathlib import Path
import struct
import tempfile
import unittest
from frame_channel import Frames,HEADER,MAP_BYTES

class FrameTests(unittest.TestCase):
    def test_consistent_frame_and_duplicate(self):
        f=Frames('.');f.mapping=bytearray(MAP_BYTES)
        # Bottom row red, top row green; raw frame orientation is OpenGL.
        header=HEADER.pack(2,1,20,30,100,200,99,1,2,8,2,0.,0)
        f.mapping[:len(header)]=header;f.mapping[128:136]=bytes([255,0,0,255,0,255,0,255])
        state,png=f.read();self.assertEqual(state['sequence'],'20');self.assertTrue(state['source_verified'])
        import io
        from PIL import Image
        image=Image.open(io.BytesIO(png));self.assertEqual(image.getpixel((0,0)),(0,255,0,255));self.assertEqual(image.getpixel((0,1)),(255,0,0,255))
        self.assertIsNone(f.read())
    def test_write_in_progress_rejected(self):
        f=Frames('.');f.mapping=bytearray(MAP_BYTES);struct.pack_into('<QQ',f.mapping,0,3,1)
        self.assertIsNone(f.read())
    def test_oversize_rejected(self):
        f=Frames('.');f.mapping=bytearray(MAP_BYTES);f.mapping[:88]=HEADER.pack(2,1,0,0,0,0,0,9000,9000,9000*9000*4,0,0.,0)
        with self.assertRaises(ValueError):f.read()

if __name__=='__main__':unittest.main()
