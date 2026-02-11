import { useEffect, useMemo, useRef, useState } from "react";
import Hls from "hls.js";

const VIEWS = {
  HOME: "home",
  CAMERA: "camera",
  LIVE: "live",
  PLAYBACK: "playback"
};

export default function App() {
  const [view, setView] = useState(VIEWS.HOME);
  const [notice, setNotice] = useState("");
  const [cameras, setCameras] = useState([]);
  const [selectedCamera, setSelectedCamera] = useState(null);
  const [showForm, setShowForm] = useState(false);
  const videoRef = useRef(null);
  const [formData, setFormData] = useState({
    name: "",
    rtspUrl: "",
    maxPlaybackMinutes: ""
  });

  const apiBase = "http://localhost:8000";
  const addCameraApi = `${apiBase}/api/cameras`;
  const liveHlsUrl = selectedCamera
    ? `${apiBase}/api/cameras/${selectedCamera.id}/live.m3u8`
    : "";
  const playbackHlsUrl = selectedCamera
    ? `${apiBase}/api/cameras/${selectedCamera.id}/playback.m3u8`
    : "";

  const title = useMemo(() => {
    if (view === VIEWS.LIVE) return "Live";
    if (view === VIEWS.PLAYBACK) return "Playback";
    if (view === VIEWS.CAMERA) return "Camera";
    return "Camera Portal";
  }, [view]);

  const handleNav = (next) => {
    setNotice("");
    setView(next);
  };

  const handleAddNewCamera = () => {
    if (cameras.length === 0) {
      setShowForm(true);
      return;
    }
    setNotice(
      "This part is not covered by the assignment demonstraction, Thanks"
    );
  };

  const handleFormChange = (event) => {
    const { name, value } = event.target;
    setFormData((prev) => ({
      ...prev,
      [name]: value
    }));
  };

  const handleFormCancel = () => {
    setShowForm(false);
    setFormData({
      name: "",
      rtspUrl: "",
      maxPlaybackMinutes: ""
    });
  };

  const handleFormSubmit = (event) => {
    event.preventDefault();
    if (!formData.rtspUrl.trim()) {
      setNotice("RTSP URL is required.");
      return;
    }
    const newCamera = {
      id: Date.now(),
      name: formData.name.trim() || `Camera ${cameras.length + 1}`,
      rtspUrl: formData.rtspUrl.trim(),
      maxPlaybackMinutes: formData.maxPlaybackMinutes.trim()
    };
    setCameras((prev) => [...prev, newCamera]);
    setShowForm(false);
    setNotice(
      `Camera saved locally. API placeholder: POST ${addCameraApi}`
    );
    setFormData({
      name: "",
      rtspUrl: "",
      maxPlaybackMinutes: ""
    });
  };

  const handleSelectCamera = (camera) => {
    setSelectedCamera(camera);
    setView(VIEWS.CAMERA);
  };

  const handleOpenLive = () => {
    setView(VIEWS.LIVE);
  };

  const handleOpenPlayback = () => {
    setView(VIEWS.PLAYBACK);
  };

  useEffect(() => {
    if (!videoRef.current) return;
    if (view !== VIEWS.LIVE && view !== VIEWS.PLAYBACK) return;
    if (!selectedCamera) return;

    const sourceUrl = view === VIEWS.LIVE ? liveHlsUrl : playbackHlsUrl;
    const video = videoRef.current;
    let hls;

    if (Hls.isSupported()) {
      hls = new Hls();
      hls.loadSource(sourceUrl);
      hls.attachMedia(video);
    } else if (video.canPlayType("application/vnd.apple.mpegurl")) {
      video.src = sourceUrl;
    }

    return () => {
      if (hls) {
        hls.destroy();
      }
      video.removeAttribute("src");
      video.load();
    };
  }, [view, selectedCamera, liveHlsUrl, playbackHlsUrl]);

  return (
    <div className="page">
      <header className="topbar glass">
        <div className="brand">Stream Control</div>
        <div className="nav">
          <button
            className={view === VIEWS.HOME ? "active" : ""}
            onClick={() => handleNav(VIEWS.HOME)}
          >
            Home
          </button>
          <button
            className={view === VIEWS.CAMERA ? "active" : ""}
            onClick={() => {
              if (cameras.length === 0) {
                setNotice("Add a camera to unlock this view.");
                setView(VIEWS.HOME);
                return;
              }
              if (selectedCamera) {
                handleNav(VIEWS.CAMERA);
              } else {
                setSelectedCamera(cameras[0]);
                setView(VIEWS.CAMERA);
              }
            }}
          >
            Cameras
          </button>
        </div>
      </header>

      <main className="content">
        <div className="hero glass">
          <h1>{title}</h1>
          <p>
            {view === VIEWS.HOME
              ? "Add a camera to unlock live and playback options."
              : "Player placeholder (HLS will be wired here)."}
          </p>
        </div>

        {view === VIEWS.HOME && (
          <div className="gallery glass">
            <div className="grid">
            {cameras.map((camera) => (
              <button
                key={camera.id}
                className="tile glass"
                onClick={() => handleSelectCamera(camera)}
              >
                <div className="tile-icon">📷</div>
                <span className="tile-title">{camera.name}</span>
                <small>{camera.rtspUrl}</small>
              </button>
            ))}
            <button className="tile add-card glass" onClick={handleAddNewCamera}>
              <div className="tile-icon">＋</div>
              <span className="tile-title">Add Camera</span>
              <small>Connect a new stream</small>
            </button>
            </div>
          </div>
        )}

        {view === VIEWS.CAMERA && selectedCamera && (
          <div className="card glass">
            <h2>Camera - {selectedCamera.name}</h2>
            <p className="muted">{selectedCamera.rtspUrl}</p>
            <div className="actions">
              <button className="primary" onClick={handleOpenLive}>
                Live
              </button>
              <button className="ghost" onClick={handleOpenPlayback}>
                Playback
              </button>
            </div>
          </div>
        )}

        {(view === VIEWS.LIVE || view === VIEWS.PLAYBACK) && (
          <div className="player-shell glass">
            <div className="player-header">
              <span>
                {view === VIEWS.LIVE ? "Live Stream" : "Playback"}
              </span>
              <button className="ghost" onClick={() => handleNav(VIEWS.HOME)}>
                Back
              </button>
            </div>
            <div className="player-placeholder">
              <video
                ref={videoRef}
                className="player-video"
                controls
                playsInline
              />
            </div>
            <small className="muted">
              HLS placeholder: {view === VIEWS.LIVE ? liveHlsUrl : playbackHlsUrl}
            </small>
          </div>
        )}

        {showForm && (
          <div className="modal-backdrop">
            <form className="modal glass" onSubmit={handleFormSubmit}>
              <h2>Add Camera</h2>
              <label>
                Camera Name
                <input
                  type="text"
                  name="name"
                  value={formData.name}
                  onChange={handleFormChange}
                  placeholder="e.g. Lobby Cam"
                />
              </label>
              <label>
                RTSP URL
                <input
                  type="text"
                  name="rtspUrl"
                  value={formData.rtspUrl}
                  onChange={handleFormChange}
                  placeholder="rtsp://..."
                />
              </label>
              <label>
                Max Playback Minutes
                <input
                  type="number"
                  name="maxPlaybackMinutes"
                  value={formData.maxPlaybackMinutes}
                  onChange={handleFormChange}
                  placeholder="e.g. 30"
                  min="1"
                />
              </label>
              <div className="actions">
                <button className="primary" type="submit">
                  Add
                </button>
                <button
                  className="ghost"
                  type="button"
                  onClick={handleFormCancel}
                >
                  Cancel
                </button>
              </div>
            </form>
          </div>
        )}

        {notice && (
          <div className="notice glass">
            {notice}
          </div>
        )}
      </main>
    </div>
  );
}
