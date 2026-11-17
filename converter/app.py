import os, subprocess, tempfile
from fastapi import FastAPI, Header, HTTPException, Request
from fastapi.responses import Response
from PIL import Image, ImageOps

W, H = 128, 64
FPS = 8
MAX_SECONDS = 8            # 64 frames max -> 64 KB in the ESP32's RAM
AUTH = os.environ["CONVERTER_KEY"]

app = FastAPI()

@app.get("/")
def health():
    return "ok"

@app.post("/convert")
async def convert(request: Request, kind: str = "video", x_auth: str = Header(None)):
    if x_auth != AUTH:
        raise HTTPException(403)
    data = await request.body()
    with tempfile.NamedTemporaryFile() as f:
        f.write(data); f.flush()
        cmd = ["ffmpeg", "-v", "error", "-i", f.name]
        if kind == "image":
            cmd += ["-frames:v", "1"]
            vf = ""
        else:
            cmd += ["-t", str(MAX_SECONDS)]
            vf = f"fps={FPS},"
        vf += (f"scale={W}:{H}:force_original_aspect_ratio=decrease,"
               f"pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:black,format=gray")
        cmd += ["-vf", vf, "-f", "rawvideo", "-pix_fmt", "gray", "-"]
        p = subprocess.run(cmd, capture_output=True, timeout=120)
    if p.returncode != 0 or not p.stdout:
        raise HTTPException(422, p.stderr.decode()[:200] or "no frames")

    fsz = W * H
    frames = []
    for i in range(0, len(p.stdout) - fsz + 1, fsz):
        im = Image.frombytes("L", (W, H), p.stdout[i:i + fsz])
        im = ImageOps.autocontrast(im)
        frames.append(im.convert("1").tobytes())   # Floyd-Steinberg, 1bpp MSB-first = Adafruit drawBitmap
    n = len(frames)
    mtype = 1 if (kind == "image" or n == 1) else 2
    head = bytes([mtype, FPS if mtype == 2 else 0, n & 255, n >> 8])
    return Response(head + b"".join(frames), media_type="application/octet-stream")
