import spdlog
import unittest
import os
import signal
import subprocess
import sys
import tempfile
import time

from spdlog import ConsoleLogger, FileLogger, RotatingLogger, DailyLogger, LogLevel
    
def set_log_level(logger, level):
    print("Setting Log level to %d" % level)
    logger.set_level(level)


def log_msg(logger):
    logger.trace('I am Trace')
    logger.debug('I am Debug')
    logger.info('I am Info')
    logger.warn('I am Warning')
    logger.error('I am Error')
    logger.critical('I am Critical')


class SpdLogTest(unittest.TestCase):
    def test_console_logger(self):
        name = 'Console Logger'
        tf = (True, False)
        for multithreaded in tf:
            for stdout in tf:
                for colored in tf:
                    logger = ConsoleLogger(name, multithreaded, stdout, colored) 
                    logger.info('I am a console log test.')
                    spdlog.drop(name)

    def test_drop(self):
        name = 'Console Logger'
        for i in range(10):
            tf = (True, False)
            for multithreaded in tf:
                for stdout in tf:
                    for colored in tf:
                        logger = ConsoleLogger(name, multithreaded, stdout, colored) 
                        spdlog.drop(logger.name())
    def test_log_level(self):
        logger = ConsoleLogger('Logger', False, True, True)
        for level in (LogLevel.TRACE, LogLevel.DEBUG, LogLevel.INFO, LogLevel.WARN,
                LogLevel.ERR, LogLevel.CRITICAL):
            set_log_level(logger, level)
            log_msg(logger)

    @unittest.skipUnless(hasattr(signal, "SIGTERM"), "SIGTERM not available on this platform")
    def test_flush_on_sigterm_without_flush_on_setting(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            logfile = os.path.join(tmpdir, "sigterm_flush.log")
            script = (
                "import time\n"
                "import spdlog\n"
                "logger = spdlog.FileLogger('sigterm_test_logger', r'" + logfile + "', False, True)\n"
                "logger.info('line-before-sigterm')\n"
                "time.sleep(30)\n"
            )

            proc = subprocess.Popen([sys.executable, "-c", script])
            time.sleep(0.5)
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=5)

            self.assertEqual(proc.returncode, -signal.SIGTERM)
            with open(logfile, "r", encoding="utf-8") as f:
                self.assertIn("line-before-sigterm", f.read())
    
    
       
if __name__ == "__main__":
    unittest.main()
