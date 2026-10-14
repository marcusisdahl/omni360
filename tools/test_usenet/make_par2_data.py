"""
Makes par2 test data for test_usenet.cpp with par2cmdline itself - the
reference implementation - so the repair is checked against the real format:

    python make_par2_data.py C:\\path\\to\\par2.exe [folder]   (default: .\\data)

It needs make_test_data.py's data first (the new-style RAR set).

  par2\\good\\       the RAR set as posted, with game.par2 and its volumes
                    (64KB slices, 40 recovery slices)
  par2\\damaged\\    a copy as a bad download leaves it:
                      game.part3.rar   100KB in the middle zeroed - an article that never came
                      game.part7.rar   its last 200KB missing - the last articles gone
                      game.part9.rar   not there at all
                      game.part5.rar   there, but as "a8f7e6d5c4b3" - posted under a made-up name
                    and the par2 files, with the index first

par2.exe is the official build from github.com/Parchive/par2cmdline.
"""
import os, shutil, subprocess, sys

PAR2 = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
source = os.path.join(OUT, "new")
good = os.path.join(OUT, "par2", "good")
damaged = os.path.join(OUT, "par2", "damaged")

for d in (good, damaged):
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)

volumes = sorted(f for f in os.listdir(source) if f.endswith(".rar"))
for v in volumes:
    shutil.copy(os.path.join(source, v), good)

subprocess.run([PAR2, "create", "-q", "-s65536", "-c40", "-n4", os.path.join(good, "game.par2")]
               + [os.path.join(good, v) for v in volumes], check=True, cwd=good)

for f in os.listdir(good):
    if f == "game.part9.rar":
        continue
    target = "a8f7e6d5c4b3" if f == "game.part5.rar" else f
    shutil.copy(os.path.join(good, f), os.path.join(damaged, target))

with open(os.path.join(damaged, "game.part3.rar"), "r+b") as f:
    f.seek(400000)
    f.write(b"\0" * 100000)
size = os.path.getsize(os.path.join(damaged, "game.part7.rar"))
with open(os.path.join(damaged, "game.part7.rar"), "r+b") as f:
    f.truncate(size - 200000)

print("wrote", os.path.join(OUT, "par2"), "-", ", ".join(sorted(os.listdir(good))))
