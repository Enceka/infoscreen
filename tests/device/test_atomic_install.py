"""Install fixtures into a temporary root, including an executing ELF inode."""
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

TOOL=Path(__file__).resolve().parents[2]/'root/usr/libexec/e5-infoscreen/install-files'

class AtomicInstall(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)/'target';self.root.mkdir()
    def archive(self,name,data=b'new',kind=None):
        archive=Path(self.tmp.name)/'update.tar.gz'
        with tarfile.open(archive,'w:gz') as out:
            item=tarfile.TarInfo(name);item.mode=0o755;item.size=len(data)
            if kind:item.type=kind;item.linkname='/etc/passwd';item.size=0;out.addfile(item)
            else:out.addfile(item,io.BytesIO(data))
        return archive
    def install(self,archive):
        return subprocess.run(['sh',str(TOOL),str(archive),str(self.root)],capture_output=True,text=True)
    def test_running_native_program_is_replaced_without_truncation(self):
        path=self.root/'usr/libexec/native-test';path.parent.mkdir(parents=True)
        path.write_bytes(Path('/bin/sleep').read_bytes());path.chmod(0o755)
        old=subprocess.Popen([str(path),'30'])
        try:
            new=Path('/bin/true').read_bytes()
            result=self.install(self.archive('usr/libexec/native-test',new))
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIsNone(old.poll());self.assertEqual(path.read_bytes(),new)
            self.assertEqual(subprocess.run([str(path)]).returncode,0)
            self.assertFalse(list(self.root.rglob('*.infoscreen-new.*')))
        finally:old.terminate();old.wait()
    def test_user_plugin_web_link_is_not_written_through(self):
        private=self.root/'etc/e5-infoscreen/plugins/phone';private.mkdir(parents=True)
        (private/'manifest.json').write_text('{}');(private/'phone.js').write_text('keep')
        web=self.root/'usr/share/e5-infoscreen/www/plugins/phone';web.parent.mkdir(parents=True)
        web.symlink_to(private,target_is_directory=True)
        result=self.install(self.archive('usr/share/e5-infoscreen/www/plugins/phone/phone.js'))
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual((private/'phone.js').read_text(),'keep')
    def test_settings_and_traversal_and_links_are_rejected(self):
        for path,kind in [('etc/config/e5-infoscreen',None),('../outside',None),
                          ('usr/share/e5-infoscreen/link',tarfile.SYMTYPE),('lib/not-screen',None)]:
            with self.subTest(path=path):self.assertNotEqual(self.install(self.archive(path,kind=kind)).returncode,0)
    def test_mode_and_restore_existing_file(self):
        path=self.root/'usr/libexec/test';path.parent.mkdir(parents=True);path.write_text('old')
        result=self.install(self.archive('usr/libexec/test',b'restored'))
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(path.read_bytes(),b'restored');self.assertEqual(path.stat().st_mode&0o777,0o755)

if __name__=='__main__':unittest.main()
