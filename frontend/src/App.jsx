import { useCallback, useEffect, useMemo, useRef, useState } from "react";
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
  const [loading, setLoading] = useState(false);
  const [isSubmitting, setIsSubmitting] = useState(false);
  const [liveQuality, setLiveQuality] = useState("copy");
  const [playbackQuality, setPlaybackQuality] = useState("high");
  const videoRef = useRef(null);
  const [formData, setFormData] = useState({
    name: "",
    rtspUrl: "",
    maxPlaybackMinutes: ""
  });

  const apiBase = import.meta.env.VITE_API_BASE || "http://localhost:8000";
  const addCameraApi = `${apiBase}/api/cameras`;
  const liveHlsUrl = selectedCamera
    ? `${apiBase}/api/cameras/${selectedCamera.id}/live.m3u8?quality=${liveQuality}`
    : "";
  const playbackHlsUrl = selectedCamera
    ? `${apiBase}/api/cameras/${selectedCamera.id}/playback.m3u8?quality=${playbackQuality}`
    : "";
  const liveQualityOptions = [
    { value: "copy", label: "Original" },
    { value: "low", label: "Low" },
    { value: "mid", label: "Mid" },
    { value: "high", label: "High" }
  ];
  const playbackQualityOptions = [
    { value: "high", label: "High" },
    { value: "mid", label: "Mid" },
    { value: "low", label: "Low" },
    { value: "copy", label: "Original" }
  ];

  const mapCamera = useCallback(
    (apiCam) => ({
      ...apiCam,
      rtspUrl: apiCam.rtsp_url,
      maxPlaybackMinutes: apiCam.max_playback_minutes
    }),
    []
  );

  const loadCameras = useCallback(async () => {
    setLoading(true);
    setNotice("");
    try {
      const res = await fetch(`${apiBase}/api/cameras`);
      if (!res.ok) {
        throw new Error(`Failed to load cameras (${res.status})`);
      }
      const data = await res.json();
      const mapped = data.map(mapCamera);
      setCameras(mapped);
      if (!selectedCamera && mapped.length > 0) {
        setSelectedCamera(mapped[0]);
      }
    } catch (err) {
      setNotice(err.message || "Unable to load cameras.");
    } finally {
      setLoading(false);
    }
  }, [apiBase, selectedCamera, mapCamera]);

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
    setNotice("");
    setShowForm(true);
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

  const handleFormSubmit = async (event) => {
    event.preventDefault();
    if (!formData.rtspUrl.trim()) {
      setNotice("RTSP URL is required.");
      return;
    }
    const maxPlayback = formData.maxPlaybackMinutes.trim()
      ? Number(formData.maxPlaybackMinutes)
      : undefined;

    const payload = {
      name: formData.name.trim() || `Camera ${cameras.length + 1}`,
      rtsp_url: formData.rtspUrl.trim(),
      max_playback_minutes: maxPlayback || undefined
    };

    setIsSubmitting(true);
    setNotice("");
    try {
      const res = await fetch(addCameraApi, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload)
      });

      if (!res.ok) {
        throw new Error(`Failed to add camera (${res.status})`);
      }

      const created = mapCamera(await res.json());
      setCameras((prev) => [...prev, created]);
      setSelectedCamera(created);
      setView(VIEWS.CAMERA);
      setShowForm(false);
      setFormData({ name: "", rtspUrl: "", maxPlaybackMinutes: "" });
      setNotice("Camera added and streaming started.");
    } catch (err) {
      setNotice(err.message || "Unable to add camera.");
    } finally {
      setIsSubmitting(false);
    }
  };

  const handleSelectCamera = (camera) => {
    setSelectedCamera(camera);
    setView(VIEWS.CAMERA);
  };

  const handleOpenLive = () => {
    setNotice("");
    setView(VIEWS.LIVE);
  };

  const handleOpenPlayback = () => {
    setNotice("");
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

  useEffect(() => {
    loadCameras();
  }, [loadCameras]);

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
            <div className="player-actions">
              {view === VIEWS.LIVE && (
                <label className="select-row">
                  Quality
                  <select
                    className="select"
                    value={liveQuality}
                    onChange={(e) => setLiveQuality(e.target.value)}
                  >
                    {liveQualityOptions.map((q) => (
                      <option key={q.value} value={q.value}>
                        {q.label}
                      </option>
                    ))}
                  </select>
                </label>
              )}

              {view === VIEWS.PLAYBACK && (
                <label className="select-row">
                  Quality
                  <select
                    className="select"
                    value={playbackQuality}
                    onChange={(e) => setPlaybackQuality(e.target.value)}
                  >
                    {playbackQualityOptions.map((q) => (
                      <option key={q.value} value={q.value}>
                        {q.label}
                      </option>
                    ))}
                  </select>
                </label>
              )}
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
              HLS: {view === VIEWS.LIVE ? liveHlsUrl : playbackHlsUrl}
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

        {loading && (
          <div className="notice glass">Loading cameras...</div>
        )}
      </main>
    </div>
  );
}
