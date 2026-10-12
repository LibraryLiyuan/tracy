import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch
from server import Broker

class NavigationTests(unittest.TestCase):
    def broker(self):
        b=Broker.__new__(Broker);b.process=None;b.channel=Path('unused');b.current={'sequence':'20','view_id':'cleanup'}
        b.compressed_frame=({'navigation':'19','sequence':'100','view_id':'cleanup','frame':'1'},b'PNG')
        b.ready_seen_at=time.monotonic()-6;b.frame_error=None;b.frame_condition=threading.Condition()
        return b
    def test_stale_frame_never_satisfies_new_navigation(self):
        b=self.broker()
        self.assertIsNone(b.wait_frame('0','cleanup','20',timeout=0))
        self.assertIsNotNone(b.wait_frame('0','cleanup','19',timeout=0))
    def test_ready_without_matching_frame_has_actionable_error(self):
        b=self.broker()
        with patch('server.read',return_value={'phase':'ready','sequence':'20','view_id':'cleanup'}):
            status=b.status();self.assertFalse(status['frame_channel_ready']);self.assertIn('frame_channel_error',status)
            self.assertNotIn('sequence',b.live())
    def test_matching_frame_clears_missing_frame_error(self):
        b=self.broker();b.compressed_frame[0]['navigation']='20'
        with patch('server.read',return_value={'phase':'ready','sequence':'20','view_id':'cleanup'}):
            status=b.status();self.assertTrue(status['frame_channel_ready']);self.assertNotIn('frame_channel_error',status)

if __name__=='__main__':unittest.main()
