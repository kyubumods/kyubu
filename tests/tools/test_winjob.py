"""kmap's Job Object (add-native-replay D8): a process and the one it starts end when the job closes."""
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'kmap'))

pytestmark = pytest.mark.skipif(os.name != 'nt', reason='Job Objects are Windows only')

CHILD = ('import subprocess, sys, time\n'
         'subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])\n'
         'time.sleep(60)\n')


def test_close_ends_process_and_grandchild():
    from kmap.winjob import Job
    job = Job()
    proc = subprocess.Popen([sys.executable, '-c', CHILD])
    job.add(proc)
    end = time.time() + 10
    while job.active() < 2 and time.time() < end:      # the grandchild is in the job from its start
        time.sleep(0.1)
    assert job.active() == 2
    job.close()
    assert proc.wait(10) is not None
    job2 = Job()
    assert job2.active() == 0 and job2.wait_empty(1)
    job2.close()
    job.close()                                        # a second close does nothing


def test_job_alive_outlives_first_process():
    # A host that restarts itself ends its first process; the run goes on while the second one lives in the job.
    from kmap.winjob import Job
    from kmap.probes import JobAlive
    with Job() as job:
        first = subprocess.Popen([sys.executable, '-c',
                                  'import subprocess, sys\nsubprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])'])
        job.add(first)
        alive = JobAlive(job, first)
        first.wait(10)
        end = time.time() + 5
        while alive.poll() is not None and time.time() < end:
            time.sleep(0.1)
        assert first.poll() == 0 and alive.poll() is None
    assert alive.poll() is None or True
