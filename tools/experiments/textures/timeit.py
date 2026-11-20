import resource, subprocess, sys, time
out = sys.argv[1]; t = time.monotonic()
r = subprocess.run(sys.argv[2:], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
e = time.monotonic() - t
open(out, 'w').write(f"{e:.2f} {resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss}\n")
sys.exit(r.returncode)
