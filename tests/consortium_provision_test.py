#!/usr/bin/env python3
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('consortium_provision', ROOT/'tools/consortium_provision.py')
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)
IDENTITY = '1234567890abcdef1234567890abcdef'
class Port:
    instance = None
    response = b'OK'
    def __init__(self, **kwargs): self.writes=[]; self.lines=[]; Port.instance=self
    def open(self): pass
    def __enter__(self): return self
    def __exit__(self, *args): pass
    def write(self, data):
        self.writes.append(data)
        if data.endswith(b'CONSORTIUM ID\n'): self.lines.append(('AMITY CONSORTIUM ID '+IDENTITY).encode())
        elif b' ROTATE ' in data: self.lines.append(b'AMITY CONSORTIUM ROTATE '+Port.response)
        else: self.lines.append(b'AMITY CONSORTIUM PROVISION '+Port.response)
    def readline(self): return self.lines.pop(0) if self.lines else b''
class ProvisionTest(unittest.TestCase):
    def setUp(self): Port.response=b'OK'; Port.instance=None
    def invoke(self, path, *extra):
        out=io.StringIO()
        with patch.object(sys,'argv',['consortium_provision','--port','physical-usb','--output',str(path),*extra]), patch.dict(sys.modules,{'serial':types.SimpleNamespace(Serial=Port)}), contextlib.redirect_stdout(out):
            provision.main()
        return out.getvalue()
    def test_provision_and_rotation_keep_identity_and_private_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            original=Path(directory)/'original.code'; rotated=Path(directory)/'new.code'
            self.invoke(original); old_code=original.read_text().strip()
            output=self.invoke(rotated,'--rotate','--confirm-id',IDENTITY)
            code=rotated.read_text().strip(); self.assertNotEqual(code,old_code); self.assertEqual(code.split(':')[1],IDENTITY)
            self.assertEqual(rotated.stat().st_mode&0o777,0o600)
            command=Port.instance.writes[-1].decode().strip(); self.assertTrue(command.startswith('AMITY CONSORTIUM ROTATE '+IDENTITY+' '))
            record=command.split()[-1]; salt,actual=record.split(':'); password=code.split(':')[2]
            self.assertEqual(actual,provision.verifier(IDENTITY,password,bytes.fromhex(salt)).hex())
            self.assertNotEqual(actual,provision.verifier(IDENTITY,old_code.split(':')[2],bytes.fromhex(salt)).hex())
            self.assertNotIn(password,command); self.assertNotIn(password,output); self.assertNotIn(actual,output)
            self.assertEqual(original.read_text().strip(),old_code)
    def test_exact_attached_identity_precedes_secret_or_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'no.code'
            with self.assertRaisesRegex(RuntimeError,'differs'):
                self.invoke(path,'--rotate','--confirm-id','f'*32)
            self.assertFalse(path.exists()); self.assertEqual(len(Port.instance.writes),1)
    def test_rotation_requires_explicit_confirmation_before_usb(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stderr(io.StringIO()):
            for arguments in [('--rotate',),('--confirm-id',IDENTITY),('--rotate','--confirm-id','0'*32)]:
                with self.assertRaises(SystemExit): self.invoke(Path(directory)/'no.code',*arguments)
                self.assertIsNone(Port.instance)
    def test_unknown_outcome_retains_new_code_and_preserves_old_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            original=Path(directory)/'original.code'; fresh=Path(directory)/'attempt.code'
            self.invoke(original); before=original.read_bytes(); Port.response=b'FAILED'
            with self.assertRaisesRegex(RuntimeError,'do not assume'):
                self.invoke(fresh,'--rotate','--confirm-id',IDENTITY)
            self.assertTrue(fresh.exists()); self.assertEqual(original.read_bytes(),before)
if __name__=='__main__': unittest.main()
