import copy
import unittest
from prepare import store_id
from export import validate

class RegistryTests(unittest.TestCase):
    def test_refs_reject_wrong_capture_and_type(self):
        fingerprint='abcdef0123456789'+'0'*48
        self.assertEqual(store_id('tracy:v1:abcdef0123456789:cpu-zone:ff','cpu-zone',fingerprint),'255')
        for value in ['tracy:v1:1111111111111111:cpu-zone:ff','tracy:v1:abcdef0123456789:gpu-zone:ff','0x123']:
            with self.assertRaises(ValueError):store_id(value,'cpu-zone',fingerprint)

    def test_complete_receipt_and_wrong_identity(self):
        loc={'frame_set':'Player.Frame','frame_index':30,'frame_begin_ns':'100','frame_end_ns':'200','focus_name':'Main','focus_gpu':False,'view_begin_ns':'90','view_end_ns':'210','event':{'query_index':'15','gpu':False,'begin_ns':'110','end_ns':'190','name':'Cost'}}
        config={'capture':{'sha256':'a'*64,'revision':17}}
        state={**loc,'event':{**loc['event'],'store_id':'900'},'ok':True,'layout_ready':True,'content_verification_state':2,'source_sha256':'a'*64,'revision':'17','actual_view_begin_ns':'90','actual_view_end_ns':'210','selected_event_visible':True}
        validate(state,{'location':loc},config)
        for key,value in [('source_sha256','b'*64),('frame_index',31),('content_verification_state',1),('selected_event_visible',False),('actual_view_begin_ns','100')]:
            bad=copy.deepcopy(state);bad[key]=value
            with self.assertRaises(ValueError):validate(bad,{'location':loc},config)

if __name__=='__main__':unittest.main()
