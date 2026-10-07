"""Installer boundary checks with a disposable fake Docker and HTTP readiness probe.

These tests verify local orchestration and preservation, not a container build.
The real container install is separately exercised on the acceptance server.
"""
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ServerInstallerTests(unittest.TestCase):
    def setUp(self):
        self.sandbox = tempfile.TemporaryDirectory()
        self.addCleanup(self.sandbox.cleanup)
        self.root = Path(self.sandbox.name) / 'project with spaces'
        self.server = self.root / 'server'
        (self.root / 'scripts').mkdir(parents=True)
        self.server.mkdir()
        self.script = self.root / 'scripts' / 'install-server.sh'
        shutil.copyfile(ROOT / 'scripts' / 'install-server.sh', self.script)
        (self.server / 'compose.yaml').write_text('services: {}\n')
        self.bin = Path(self.sandbox.name) / 'commands'
        self.bin.mkdir()
        self.log = Path(self.sandbox.name) / 'docker-arguments.jsonl'
        # Only the explicit command list is visible to the script. A missing curl
        # cannot be accidentally satisfied by the developer computer's PATH.
        for command in ('dirname', 'awk', 'sed', 'chmod', 'mkdir', 'mktemp', 'mv', 'openssl', 'cat', 'id'):
            path = shutil.which(command)
            self.assertIsNotNone(path, command)
            (self.bin / command).symlink_to(path)
        self.fake('docker', """
import json,os,sys
args=sys.argv[1:]
with open(os.environ['INSTALL_TEST_LOG'],'a') as out:
    out.write(json.dumps(args)+'\\n')
fail=os.environ.get('INSTALL_TEST_FAIL','')
if ((fail=='daemon' and args==['info']) or
    (fail=='config' and 'config' in args) or
    (fail=='start' and 'up' in args)):
    raise SystemExit(1)
if 'up' in args and os.environ.get('INSTALL_TEST_EXPECT_KEY'):
    if os.environ.get('PODCAST_SETUP_KEY') != os.environ['INSTALL_TEST_EXPECT_KEY']:
        raise SystemExit(2)
if 'config' in args or 'up' in args:
    for field in ('UID','GID'):
        expected=os.environ.get('INSTALL_TEST_EXPECT_'+field)
        if expected is not None and os.environ.get('PODCAST_'+field) != expected:
            raise SystemExit(3)
""")
        self.fake('curl', """
import os
print('{"ok":false}' if os.environ.get('INSTALL_TEST_HEALTH')=='fail' else '{"ok":true}')
""")
        self.fake('sleep', '')
        self.fake('ip', "print('1.1.1.1 via 192.168.16.1 dev eth0 src 192.168.16.20 uid 1000')")

    def fake(self, name, program):
        path = self.bin / name
        path.write_text('#!' + sys.executable + '\n' + program + '\n')
        path.chmod(0o755)

    def run_installer(self, *arguments, **overrides):
        env = {key:value for key,value in os.environ.items() if not key.startswith('PODCAST_')}
        env.update(PATH=str(self.bin), INSTALL_TEST_LOG=str(self.log), **overrides)
        return subprocess.run(['/bin/bash', str(self.script), '--no-open', *arguments],
                              env=env, capture_output=True, text=True, timeout=20)

    def configured(self, port='8899', address='http://192.168.16.20:8899', uid=None, gid=None):
        self.key = 'a' * 64
        uid = os.getuid() if uid is None else uid
        gid = os.getgid() if gid is None else gid
        value = f'PODCAST_SETUP_KEY={self.key}\nPODCAST_PORT={port}\nPODCAST_PUBLIC_URL={address}\nPODCAST_UID={uid}\nPODCAST_GID={gid}\n'
        (self.server / '.env').write_text(value)
        (self.server / 'data').mkdir(exist_ok=True)
        (self.server / 'data' / 'user-records').write_text('keep listening history')
        return value

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def assert_no_start(self):
        self.assertFalse(any('up' in call for call in self.calls()))

    def test_fresh_install_detects_lan_address_and_keeps_install_key_private(self):
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        text = (self.server / '.env').read_text()
        key = re.search(r'^PODCAST_SETUP_KEY=([0-9a-f]{64})$', text, re.M).group(1)
        self.assertIn('PODCAST_PUBLIC_URL=http://192.168.16.20:8899', text)
        self.assertIn(f'PODCAST_UID={os.getuid()}\n', text)
        self.assertIn(f'PODCAST_GID={os.getgid()}\n', text)
        for directory in (self.server/'data', self.server/'media'):
            self.assertEqual(directory.stat().st_uid, os.getuid())
            self.assertEqual(directory.stat().st_gid, os.getgid())
        self.assertNotIn(key, result.stdout + result.stderr)
        self.assertIn('/setup#install=' + key, (self.root / '.local/first-setup.html').read_text())
        for path, mode in ((self.server/'.env',0o600),(self.root/'.local',0o700),
                           (self.root/'.local/first-setup.html',0o600),(self.server/'data',0o700)):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), mode)
        self.assertTrue(any(call[-3:] == ['up','-d','--build'] for call in self.calls()))

    def test_repeat_install_preserves_key_saved_port_address_and_records(self):
        original = self.configured(port='8901', address='http://podcast.home:8901')
        first = self.run_installer()
        second = self.run_installer()
        self.assertEqual((first.returncode,second.returncode),(0,0), first.stderr + second.stderr)
        self.assertEqual((self.server/'.env').read_text(), original)
        self.assertIn('http://127.0.0.1:8901', second.stdout)
        self.assertEqual((self.server/'data/user-records').read_text(),'keep listening history')
        self.assertNotIn(self.key, first.stdout + second.stdout)

    def test_explicit_address_update_preserves_every_other_setting_and_records(self):
        old = self.configured()
        result = self.run_installer('--public-url','https://podcast.example')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual((self.server/'.env').read_text(), old.replace('http://192.168.16.20:8899','https://podcast.example'))
        self.assertTrue((self.server/'data/user-records').exists())

    def test_changing_existing_port_requires_manual_configuration_and_starts_nothing(self):
        old = self.configured()
        result = self.run_installer('--port','8900')
        self.assertNotEqual(result.returncode,0)
        self.assertEqual((self.server/'.env').read_text(),old)
        self.assert_no_start()

    def test_inherited_installation_key_cannot_override_the_preserved_configuration(self):
        old=self.configured()
        result=self.run_installer(PODCAST_SETUP_KEY='c'*64, INSTALL_TEST_EXPECT_KEY=self.key)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual((self.server/'.env').read_text(),old)
        self.assertNotIn(self.key,result.stdout+result.stderr)

    def test_unsafe_new_options_fail_before_creating_installation_files(self):
        for args in (('--port','0'),('--port','65536'),('--port','08'),
                     ('--public-url','http://host:65536'),('--public-url','http://user:pass@host'),
                     ('--public-url','http://host/path'),('--public-url','http://host/?key=secret'),
                     ('--public-url','http://host/<script>'),('--public-url','http://$(touch marker)')):
            with self.subTest(args=args):
                result=self.run_installer(*args)
                self.assertNotEqual(result.returncode,0)
                self.assertFalse((self.server/'.env').exists())
                self.assert_no_start()

    def test_invalid_or_duplicate_existing_settings_are_not_overwritten(self):
        baseline=self.configured()
        for content in (baseline.replace('PODCAST_PORT=8899','PODCAST_PORT=65536'),
                        baseline.replace('PODCAST_PORT=8899','PODCAST_PORT=08'),
                        baseline.replace('http://192.168.16.20:8899','http://host:invalid'),
                        baseline.replace('http://192.168.16.20:8899','<script>bad</script>'),
                        baseline+'PODCAST_PUBLIC_URL=http://other\n',
                        baseline+'PODCAST_SETUP_KEY='+'b'*64+'\n',
                        baseline.replace('PODCAST_PORT=8899\n','')):
            with self.subTest(content=content):
                (self.server/'.env').write_text(content)
                result=self.run_installer()
                self.assertNotEqual(result.returncode,0)
                self.assertEqual((self.server/'.env').read_text(),content)
                self.assert_no_start()
                self.assertFalse((self.root/'.local/first-setup.html').exists())

    def test_daemon_or_missing_readiness_tool_stops_before_generating_a_key(self):
        result=self.run_installer(INSTALL_TEST_FAIL='daemon')
        self.assertNotEqual(result.returncode,0)
        self.assertFalse((self.server/'.env').exists())
        (self.bin/'curl').unlink()
        result=self.run_installer()
        self.assertNotEqual(result.returncode,0)
        self.assertIn('curl',result.stderr)
        self.assertFalse((self.server/'.env').exists())
        self.assert_no_start()

    def test_failed_config_or_start_keeps_existing_state_and_no_success_page(self):
        old=self.configured()
        for failure in ('config','start'):
            with self.subTest(failure=failure):
                result=self.run_installer(INSTALL_TEST_FAIL=failure)
                self.assertNotEqual(result.returncode,0)
                self.assertEqual((self.server/'.env').read_text(),old)
                self.assertTrue((self.server/'data/user-records').exists())
                self.assertFalse((self.root/'.local/first-setup.html').exists())

    def test_http_200_with_unhealthy_body_is_not_reported_as_ready(self):
        old=self.configured()
        result=self.run_installer(INSTALL_TEST_HEALTH='fail')
        self.assertNotEqual(result.returncode,0)
        self.assertIn('尚未就绪',result.stderr)
        self.assertEqual((self.server/'.env').read_text(),old)
        self.assertFalse((self.root/'.local/first-setup.html').exists())

    def test_inherited_container_identity_cannot_override_new_or_saved_owner(self):
        result = self.run_installer(PODCAST_UID='999999', PODCAST_GID='888888',
                                    INSTALL_TEST_EXPECT_UID=str(os.getuid()),
                                    INSTALL_TEST_EXPECT_GID=str(os.getgid()))
        self.assertEqual(result.returncode,0,result.stderr)
        original=(self.server/'.env').read_text()
        repeated = self.run_installer(PODCAST_UID='777777', PODCAST_GID='666666',
                                      INSTALL_TEST_EXPECT_UID=str(os.getuid()),
                                      INSTALL_TEST_EXPECT_GID=str(os.getgid()))
        self.assertEqual(repeated.returncode,0,repeated.stderr)
        self.assertEqual((self.server/'.env').read_text(),original)

    def test_invalid_duplicate_or_missing_saved_identities_do_not_modify_state(self):
        baseline=self.configured()
        for field,actual in (('UID',os.getuid()),('GID',os.getgid())):
            for invalid in ('-1','+1','01','1.0','4294967295','999999999999','abc','1000;touch marker','1000 1001',''):
                content=baseline.replace(f'PODCAST_{field}={actual}\n',f'PODCAST_{field}={invalid}\n')
                with self.subTest(field=field,invalid=invalid):
                    (self.server/'.env').write_text(content)
                    result=self.run_installer('--public-url','https://new.example')
                    self.assertNotEqual(result.returncode,0)
                    self.assertEqual((self.server/'.env').read_text(),content)
                    self.assertEqual((self.server/'data/user-records').read_text(),'keep listening history')
                    self.assert_no_start()
                    self.assertFalse((self.root/'.local/first-setup.html').exists())
            for content in (baseline+f'PODCAST_{field}={actual}\n',baseline.replace(f'PODCAST_{field}={actual}\n','')):
                with self.subTest(field=field,content='duplicate-or-missing'):
                    (self.server/'.env').write_text(content)
                    result=self.run_installer()
                    self.assertNotEqual(result.returncode,0)
                    self.assertEqual((self.server/'.env').read_text(),content)
                    self.assert_no_start()

    def test_different_saved_owner_is_refused_without_changing_records_or_address(self):
        for uid,gid in ((os.getuid()+1,os.getgid()),(os.getuid(),os.getgid()+1)):
            with self.subTest(uid=uid,gid=gid):
                original=self.configured(uid=uid,gid=gid)
                owner=(self.server/'data').stat().st_uid
                result=self.run_installer('--public-url','https://new.example')
                self.assertNotEqual(result.returncode,0)
                self.assertIn('另一运行身份',result.stderr)
                self.assertEqual((self.server/'.env').read_text(),original)
                self.assertEqual((self.server/'data').stat().st_uid,owner)
                self.assertEqual((self.server/'data/user-records').read_text(),'keep listening history')
                self.assert_no_start()


if __name__ == '__main__':
    unittest.main()
