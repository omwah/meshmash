# PlatformIO pre-build hook: generate fonts (for the environment's Unicode
# profile), and (for device builds) prepare the patched MeshCore tree. Runs
# inside PlatformIO's Python.
import os
import subprocess
import sys

Import("env")  # noqa: F821  (provided by PlatformIO)

root = env.subst("$PROJECT_DIR")  # noqa: F821
# Unicode coverage: MC_UNICODE in the environment wins over custom_unicode here.
gen_env = dict(os.environ)
gen_env.setdefault("MC_UNICODE", env.GetProjectOption("custom_unicode", ""))  # noqa: F821
print("Unicode coverage for %s: %s" % (env.subst("$PIOENV"), gen_env["MC_UNICODE"] or "(default)"))  # noqa: F821
subprocess.check_call([sys.executable, "-I", os.path.join(root, "scripts", "gen_fonts.py")], env=gen_env)
# The version label (tag, commits after it, dirty): a header, so only what shows it rebuilds.
subprocess.check_call([sys.executable, "-I", os.path.join(root, "scripts", "version.py"), "--header",
                       os.path.join(root, "src", "gen", "mc_version.h")])
if env.subst("$PIOENV") not in ("sim", "sim_tdeck"):  # noqa: F821  (the mock simulator needs no MeshCore)
    subprocess.check_call([os.path.join(root, "scripts", "prepare-deps.sh")])
