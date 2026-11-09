# `python tools/kmap <command>` from a checkout, without installing the package.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kmap.cli import main  # noqa: E402

sys.exit(main())
