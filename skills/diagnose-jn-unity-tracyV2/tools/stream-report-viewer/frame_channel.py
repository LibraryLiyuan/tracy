"""Read consistent native RGBA frames from a bounded Windows page-file mapping."""
import json
import mmap
from pathlib import Path
import re
import struct
import time
import zlib

HEADER=struct.Struct('<QQQQqqQIIIIdQ')
MAP_BYTES=128+8*1024*1024

def encode_rgba(pixels,width,height):
    # Filter 0 avoids the PNG library's adaptive scanline search. Colors and
    # text remain lossless; native OpenGL rows are flipped exactly once.
    stride=width*4
    raw=b''.join(b'\0'+pixels[row*stride:(row+1)*stride] for row in range(height-1,-1,-1))
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw,1))+chunk(b'IEND',b'')

class Frames:
    def __init__(self,channel):self.channel=Path(channel);self.mapping=None;self.last=0
    def read(self):
        if self.mapping is None:
            value=json.loads((self.channel/'frame-mapping.json').read_text())
            if value.get('bytes')!=MAP_BYTES or value.get('name')!='Local\\TracyWeb-'+self.channel.name:raise ValueError('Unexpected native mapping')
            self.mapping=mmap.mmap(-1,MAP_BYTES,tagname=value['name'],access=mmap.ACCESS_READ)
        m=self.mapping;header=m[:HEADER.size];fields=HEADER.unpack(header)
        guard,frame,ack,nav,begin,end,selected,width,height,count,flags,native_ms,queued=fields
        if guard&1 or not frame or frame==self.last:return None
        if count!=width*height*4 or count>MAP_BYTES-128 or width<1 or height<1:raise ValueError('Invalid native framebuffer shape')
        pixels=m[128:128+count]
        if struct.unpack_from('<Q',m,0)[0]!=guard:return None
        self.last=frame;start=time.perf_counter()
        png=encode_rgba(pixels,width,height)
        state={'frame':str(frame),'sequence':str(ack),'navigation':str(nav),'begin_ns':str(begin),'end_ns':str(end),
               'selected_store_id':str(selected),'width':width,'height':height,'busy':bool(flags&1),'source_verified':bool(flags&2),
               'zone_info_visible':bool(flags&4),'selected_gpu':bool(flags&8),'native_frame_ms':round(native_ms,3),
               'input_queue_depth':queued,'encode_ms':round((time.perf_counter()-start)*1000,3),'phase':'ready'}
        return state,png
