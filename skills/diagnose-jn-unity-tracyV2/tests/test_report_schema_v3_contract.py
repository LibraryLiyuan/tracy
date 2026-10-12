"""The published JSON Schema must accept real workflow-v3 report shapes."""
import copy
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from tests.test_query_native_report_contract import query_native_analysis

SKILL = Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('pwsh'), 'PowerShell Test-Json is required for schema acceptance')
class PublishedReportSchemaTests(unittest.TestCase):
    def accepted(self, root, analysis, schema_name='analysis-result.schema.json'):
        path = root/'schema-input.json'
        path.write_text(json.dumps(analysis), encoding='utf-8')
        command = "if (Test-Json -LiteralPath '"+str(path).replace("'", "''")+"' -SchemaFile '"+str(SKILL/'schemas'/schema_name).replace("'", "''")+"' -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }"
        return subprocess.run(['pwsh', '-NoProfile', '-NonInteractive', '-Command', command],
            capture_output=True, text=True).returncode == 0

    def test_stage_report_with_actual_receipt_fields_matches_published_schema(self):
        # Removing the schema fields used by the production renderer/receipt
        # validator must fail this integration boundary, not just a text grep.
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            _,_,_,analysis,_=query_native_analysis(root)
            self.assertTrue(self.accepted(root,analysis), 'baseline contract must be valid')
            analysis['report_status']='in_progress'
            finding=analysis['candidate_findings'][0]
            finding['representative_intervals']=[]
            finding['investigation_receipts']=[{
                'evidence_id':'EVD-TIMELINE-001','purpose':'zone_tree','manifestation':'frame_cost',
                'entity_json_pointers':['/data/root'],
                'parent_chain_evidence':[{'evidence_id':'EVD-PARENT','entity_json_pointer':'/data'}],
                'frame_anchor_evidence_id':'EVD-FRAME','frame_anchor_json_pointer':'/data'}]
            self.assertTrue(self.accepted(root,analysis))
            finding['investigation_receipts'][0]['outcome']='verified_absent'
            self.assertTrue(self.accepted(root,analysis))
            finding['investigation_receipts'][0]['purpose']='pretend_investigated'
            self.assertFalse(self.accepted(root,analysis))

    def test_interval_only_candidate_is_not_forced_to_invent_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            _,_,_,analysis,_=query_native_analysis(root)
            f=analysis['candidate_findings'][0]
            f['representative_frames']=[]
            f['representative_intervals']=[{'start_ns':'10','end_ns':'20','reasons':['peak']}]
            self.assertTrue(self.accepted(root,analysis))
            f['representative_intervals']=[]
            self.assertFalse(self.accepted(root,analysis))

    def test_job_origin_identity_receipt_is_published_and_nonempty(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            _,_,_,analysis,_=query_native_analysis(root)
            receipt={'evidence_id':'EVD-JOB','purpose':'wait_completion','manifestation':'',
                'entity_json_pointers':['/data'],'job_frame_identity_evidence_id':'EVD-ORIGIN'}
            analysis['candidate_findings'][0]['investigation_receipts']=[receipt]
            self.assertTrue(self.accepted(root,analysis))
            receipt['job_frame_identity_evidence_id']=''
            self.assertFalse(self.accepted(root,analysis))

    def test_evidence_manifest_can_bind_the_original_mcp_envelope(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            _,path,_,_,_=query_native_analysis(root)
            evidence=json.loads(path.read_text(encoding='utf-8'))
            self.assertTrue(self.accepted(root,evidence,'evidence-manifest.schema.json'))
            evidence['evidence'][0]['mcp_receipt']={
                'request':{'path':'raw/request.json','sha256':'1'*64},
                'response':{'path':'raw/response.json','sha256':'2'*64}}
            self.assertTrue(self.accepted(root,evidence,'evidence-manifest.schema.json'))
            del evidence['evidence'][0]['mcp_receipt']['response']
            self.assertFalse(self.accepted(root,evidence,'evidence-manifest.schema.json'))
