from __future__ import annotations

import json
import os
import subprocess
import threading
import uuid
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import RedirectResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

APP_DIR = Path(__file__).resolve().parent
DATA_DIR = APP_DIR / "data"
STREAMS_DIR = APP_DIR / "streams"
DB_PATH = DATA_DIR / "cameras.json"

STREAMER_BIN = os.environ.get("STREAMER_BIN", str(APP_DIR.parent / "build" / "streamer"))
DEFAULT_COPY_HLS_TIME = int(os.environ.get("COPY_HLS_TIME", "0"))
DEFAULT_ENCODE_HLS_TIME = int(os.environ.get("ENCODE_HLS_TIME", "4"))
DEFAULT_COPY_KEEP_MIN = int(os.environ.get("COPY_KEEP_MIN", "0"))
DEFAULT_ENCODE_KEEP_MIN = int(os.environ.get("ENCODE_KEEP_MIN", "1"))

DATA_DIR.mkdir(parents=True, exist_ok=True)
STREAMS_DIR.mkdir(parents=True, exist_ok=True)

DB_LOCK = threading.Lock()


class CameraCreate(BaseModel):
    name: str = Field(..., min_length=1)
    rtsp_url: str = Field(..., min_length=1)
    max_playback_minutes: Optional[int] = Field(default=None, ge=1)


class CameraRecord(BaseModel):
    id: str
    name: str
    rtsp_url: str
    max_playback_minutes: Optional[int] = None
    created_at: str
    stream_dir: str
    copy_playlist: str
    low_playlist: str
    mid_playlist: str
    high_playlist: str
    process_pid: Optional[int] = None


app = FastAPI(title="Streamer API", version="0.1.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

app.mount("/streams", StaticFiles(directory=STREAMS_DIR), name="streams")


def _load_db() -> List[Dict[str, Any]]:
    if not DB_PATH.exists():
        return []
    with DB_PATH.open("r", encoding="utf-8") as f:
        return json.load(f)


def _save_db(records: List[Dict[str, Any]]) -> None:
    with DB_PATH.open("w", encoding="utf-8") as f:
        json.dump(records, f, indent=2)


def _make_stream_paths(cam_id: str) -> Dict[str, str]:
    cam_dir = STREAMS_DIR / cam_id
    cam_dir.mkdir(parents=True, exist_ok=True)
    copy_playlist = cam_dir / "index.m3u8"
    return {
        "stream_dir": str(cam_dir),
        "copy_playlist": str(copy_playlist),
        "low_playlist": str(cam_dir / "index_low.m3u8"),
        "mid_playlist": str(cam_dir / "index_mid.m3u8"),
        "high_playlist": str(cam_dir / "index_high.m3u8"),
    }


def _start_streamer(rtsp_url: str, output_path: str, max_playback_minutes: Optional[int]) -> Optional[int]:
    if not Path(STREAMER_BIN).exists():
        return None

    cmd = [
        STREAMER_BIN,
        rtsp_url,
        output_path,
        "--encode-hls-time",
        str(DEFAULT_ENCODE_HLS_TIME),
        "--copy-hls-time",
        str(DEFAULT_COPY_HLS_TIME),
        "--encode-max-keep-minutes",
        str(DEFAULT_ENCODE_KEEP_MIN),
        "--copy-max-keep-minutes",
        str(DEFAULT_COPY_KEEP_MIN),
    ]

    if max_playback_minutes:
        cmd.extend(["--encode-max-keep-minutes", str(max_playback_minutes)])

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        cwd=str(APP_DIR.parent),
    )
    return proc.pid


@app.post("/api/cameras", response_model=CameraRecord)
def create_camera(payload: CameraCreate) -> CameraRecord:
    cam_id = uuid.uuid4().hex
    paths = _make_stream_paths(cam_id)
    now = datetime.utcnow().isoformat() + "Z"

    pid = _start_streamer(
        payload.rtsp_url,
        paths["copy_playlist"],
        payload.max_playback_minutes,
    )

    record = CameraRecord(
        id=cam_id,
        name=payload.name,
        rtsp_url=payload.rtsp_url,
        max_playback_minutes=payload.max_playback_minutes,
        created_at=now,
        stream_dir=paths["stream_dir"],
        copy_playlist=paths["copy_playlist"],
        low_playlist=paths["low_playlist"],
        mid_playlist=paths["mid_playlist"],
        high_playlist=paths["high_playlist"],
        process_pid=pid,
    )

    with DB_LOCK:
        records = _load_db()
        records.append(record.model_dump())
        _save_db(records)

    return record


@app.get("/api/cameras", response_model=List[CameraRecord])
def list_cameras() -> List[CameraRecord]:
    with DB_LOCK:
        records = _load_db()
    return [CameraRecord(**rec) for rec in records]


def _find_camera(camera_id: str) -> Optional[CameraRecord]:
    with DB_LOCK:
        records = _load_db()
    for rec in records:
        if rec.get("id") == camera_id:
            return CameraRecord(**rec)
    return None


def _validate_quality(quality: Optional[str]) -> str:
    allowed = {"copy", "low", "mid", "high"}
    if not quality:
        return "copy"
    q = quality.lower()
    if q not in allowed:
        raise HTTPException(status_code=400, detail="Unsupported quality")
    return q


@app.get("/api/cameras/{camera_id}/live.m3u8")
def get_live_playlist(camera_id: str, quality: Optional[str] = None):
    camera = _find_camera(camera_id)
    if not camera:
        raise HTTPException(status_code=404, detail="Camera not found")

    q = _validate_quality(quality)

    # Live uses the copy playlist (index.m3u8). Allow quality param for compatibility.
    target = Path(camera.copy_playlist)
    if q != "copy":
        # If caller asks for specific rendition, prefer corresponding playlist if present.
        attr = f"{q}_playlist"
        target = Path(getattr(camera, attr, camera.copy_playlist))

    if not target.exists():
        raise HTTPException(status_code=404, detail="Playlist not available")

    rel_path = target.relative_to(STREAMS_DIR)
    return RedirectResponse(url=f"/streams/{rel_path.as_posix()}")


@app.get("/api/cameras/{camera_id}/playback.m3u8")
def get_playback_playlist(camera_id: str, quality: Optional[str] = None):
    camera = _find_camera(camera_id)
    if not camera:
        raise HTTPException(status_code=404, detail="Camera not found")

    q = _validate_quality(quality or "high")
    target = Path(getattr(camera, f"{q}_playlist", camera.high_playlist))

    if not target.exists():
        raise HTTPException(status_code=404, detail="Playlist not available")

    rel_path = target.relative_to(STREAMS_DIR)
    return RedirectResponse(url=f"/streams/{rel_path.as_posix()}")

