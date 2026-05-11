//! `wifi-densepose listen` — clone-and-run audio listener for the ESP32 mic.
//!
//! Pairs with the firmware's `/audio/raw_stream/{start,stop,status}` endpoints
//! (ADR-081). Binds a UDP socket on this machine, asks the node to ship raw
//! int16 PCM to it for a bounded duration, then plays the audio live (cpal)
//! and/or writes it to a WAV file (hound). Auto-stops when the duration
//! elapses or on Ctrl+C.

use std::net::{SocketAddr, UdpSocket};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::{anyhow, Context, Result};
use clap::Args;
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use serde::Deserialize;
use tracing::{debug, info, warn};

const FIRMWARE_HTTP_PORT: u16 = 8032;

#[derive(Args, Debug)]
pub struct ListenArgs {
    /// IP or hostname of the node (e.g. 192.168.0.95).
    #[arg(long)]
    pub node: String,

    /// How long to capture, in seconds (firmware caps at 300).
    #[arg(long, default_value_t = 60)]
    pub duration_s: u32,

    /// Local UDP port to bind for receiving audio. 0 = let the OS pick.
    #[arg(long, default_value_t = 0)]
    pub listen_port: u16,

    /// Write the captured stream to this WAV file. Omit to skip recording.
    #[arg(long, value_name = "PATH")]
    pub out: Option<PathBuf>,

    /// Disable live audio playback. Useful for headless / save-only runs.
    #[arg(long, default_value_t = false)]
    pub no_play: bool,

    /// HTTP port the firmware listens on. Defaults to 8032 (the OTA server).
    #[arg(long, default_value_t = FIRMWARE_HTTP_PORT)]
    pub http_port: u16,
}

#[derive(Deserialize, Debug)]
struct StartResponse {
    ok: bool,
    duration_s: u32,
    ip: String,
    port: u16,
}

#[derive(Deserialize, Debug)]
struct StatusResponse {
    sample_rate: u32,
    chunk_samples: u32,
}

pub async fn execute(args: ListenArgs) -> Result<()> {
    if args.duration_s == 0 || args.duration_s > 300 {
        return Err(anyhow!("--duration_s must be in [1, 300]"));
    }

    // 1. Bind the local UDP socket FIRST so we know the port we'll tell the firmware.
    let bind_addr: SocketAddr = format!("0.0.0.0:{}", args.listen_port)
        .parse()
        .expect("hardcoded bind addr always parses");
    let udp = UdpSocket::bind(bind_addr).context("bind local UDP socket")?;
    let local_port = udp.local_addr()?.port();
    udp.set_read_timeout(Some(Duration::from_millis(500)))?;
    info!("listening on UDP 0.0.0.0:{local_port}");

    // 2. Probe the firmware to learn its sample rate + chunk size up front.
    let http = reqwest::Client::builder()
        .timeout(Duration::from_secs(8))
        .build()?;
    let status_url = format!(
        "http://{}:{}/audio/raw_stream/status",
        args.node, args.http_port
    );
    let status: StatusResponse = http
        .get(&status_url)
        .send()
        .await
        .with_context(|| format!("GET {status_url}"))?
        .error_for_status()?
        .json()
        .await
        .context("decode raw_stream/status")?;
    let sample_rate = status.sample_rate;
    let chunk_samples = status.chunk_samples as usize;
    if sample_rate == 0 || chunk_samples == 0 {
        return Err(anyhow!(
            "node reported invalid audio config: sr={sample_rate}, chunk={chunk_samples}"
        ));
    }
    info!("node {} → sr={sample_rate} Hz, chunk={chunk_samples} samples", args.node);

    // 3. Tell firmware to start streaming to us. We don't pass `?ip=` —
    //    the firmware reads the HTTP peer address (i.e. this machine) by default.
    let start_url = format!(
        "http://{}:{}/audio/raw_stream/start?duration_s={}&port={}",
        args.node, args.http_port, args.duration_s, local_port
    );
    let started: StartResponse = http
        .post(&start_url)
        .send()
        .await
        .with_context(|| format!("POST {start_url}"))?
        .error_for_status()?
        .json()
        .await
        .context("decode raw_stream/start")?;
    if !started.ok {
        return Err(anyhow!("firmware refused start: {started:?}"));
    }
    info!(
        "firmware streaming to {}:{} for {}s",
        started.ip, started.port, started.duration_s
    );

    // 4. Set up Ctrl+C → graceful shutdown.
    let shutdown = Arc::new(AtomicBool::new(false));
    {
        let s = shutdown.clone();
        ctrlc::set_handler(move || {
            s.store(true, Ordering::SeqCst);
        })
        .ok();
    }

    // 5. Set up live playback (optional) and WAV writer (optional).
    let writer = if let Some(path) = &args.out {
        let spec = hound::WavSpec {
            channels: 1,
            sample_rate,
            bits_per_sample: 16,
            sample_format: hound::SampleFormat::Int,
        };
        let w = hound::WavWriter::create(path, spec)
            .with_context(|| format!("create WAV at {}", path.display()))?;
        info!("writing WAV → {}", path.display());
        Some(Arc::new(Mutex::new(w)))
    } else {
        None
    };

    let player = if !args.no_play {
        match Player::start(sample_rate) {
            Ok(p) => {
                info!("live playback active (sr={sample_rate} Hz)");
                Some(p)
            }
            Err(e) => {
                warn!("could not start audio playback ({e}); --no-play to silence");
                None
            }
        }
    } else {
        None
    };

    if writer.is_none() && player.is_none() {
        return Err(anyhow!(
            "nothing to do: no --out specified and --no-play set; pick at least one"
        ));
    }

    // 6. Receive loop. The firmware sends raw int16 LE chunks.
    let chunk_bytes = chunk_samples * 2;
    let max_packet = chunk_bytes.max(2048);
    let mut buf = vec![0u8; max_packet];
    let mut samples = Vec::<i16>::with_capacity(chunk_samples);
    let mut packets_seen = 0u64;
    let mut bytes_seen = 0u64;
    let stop_url = format!(
        "http://{}:{}/audio/raw_stream/stop",
        args.node, args.http_port
    );
    let deadline = Instant::now() + Duration::from_secs(args.duration_s as u64 + 5);

    while !shutdown.load(Ordering::SeqCst) && Instant::now() < deadline {
        let (n, _src) = match udp.recv_from(&mut buf) {
            Ok(x) => x,
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock
                  || e.kind() == std::io::ErrorKind::TimedOut => continue,
            Err(e) => return Err(anyhow!("recv_from: {e}")),
        };
        if n == 0 || n % 2 != 0 {
            debug!("dropping odd-length packet ({n} bytes)");
            continue;
        }
        packets_seen += 1;
        bytes_seen += n as u64;

        samples.clear();
        samples.reserve(n / 2);
        for chunk in buf[..n].chunks_exact(2) {
            samples.push(i16::from_le_bytes([chunk[0], chunk[1]]));
        }

        if let Some(p) = &player {
            p.push(&samples);
        }
        if let Some(w) = &writer {
            let mut g = w.lock().expect("WAV writer mutex poisoned");
            for &s in &samples {
                g.write_sample(s).context("WAV write")?;
            }
        }
    }

    // 7. Tell the node to stop (best-effort; deadline auto-stops anyway).
    if let Err(e) = http.post(&stop_url).send().await {
        debug!("POST /stop failed (deadline already elapsed?): {e}");
    }

    // 8. Drain WAV.
    if let Some(w) = writer {
        let writer = Arc::try_unwrap(w)
            .map_err(|_| anyhow!("WAV writer still has shared refs"))?
            .into_inner()
            .expect("WAV writer mutex");
        let dur_s = writer.duration() as f32 / sample_rate as f32;
        writer.finalize().context("finalize WAV")?;
        info!("wrote WAV: {dur_s:.2}s");
    }
    drop(player);

    info!(
        "received {} packets / {} KB",
        packets_seen,
        bytes_seen / 1024
    );
    if packets_seen == 0 {
        warn!("no packets arrived — check firewall, RSSI, and ?ip= override");
    }

    Ok(())
}

/// Live-playback wrapper around cpal. Producer pushes int16 samples; the
/// audio thread converts to f32 and feeds the OS audio device.
struct Player {
    _stream: cpal::Stream,
    queue: Arc<Mutex<std::collections::VecDeque<f32>>>,
}

impl Player {
    fn start(sample_rate: u32) -> Result<Self> {
        let host = cpal::default_host();
        let device = host
            .default_output_device()
            .ok_or_else(|| anyhow!("no default audio output device"))?;
        let mut configs = device
            .supported_output_configs()
            .context("query supported configs")?;
        let target = configs
            .find(|c| c.channels() >= 1 && c.sample_format() == cpal::SampleFormat::F32)
            .ok_or_else(|| anyhow!("no f32 output config available"))?;
        let mut config = target.with_max_sample_rate().config();
        // cpal will resample if the device doesn't support our SR exactly.
        config.sample_rate = cpal::SampleRate(sample_rate);
        config.channels = 1;

        let queue: Arc<Mutex<std::collections::VecDeque<f32>>> =
            Arc::new(Mutex::new(std::collections::VecDeque::with_capacity(
                sample_rate as usize,
            )));
        let q = queue.clone();
        let err_fn = |e| warn!("cpal stream error: {e}");
        let stream = device
            .build_output_stream(
                &config,
                move |out: &mut [f32], _info: &cpal::OutputCallbackInfo| {
                    let mut g = q.lock().expect("queue mutex poisoned");
                    for slot in out.iter_mut() {
                        *slot = g.pop_front().unwrap_or(0.0);
                    }
                },
                err_fn,
                None,
            )
            .context("build cpal output stream")?;
        stream.play().context("start cpal stream")?;
        Ok(Self {
            _stream: stream,
            queue,
        })
    }

    fn push(&self, samples: &[i16]) {
        let mut g = self.queue.lock().expect("queue mutex poisoned");
        // Cap latency at ~500ms — drop oldest if the audio device is slow.
        // 500ms at any reasonable sample rate << total buffer reservation.
        let max_buffered = 24_000usize;
        let len = g.len();
        if len > max_buffered {
            g.drain(..(len - max_buffered));
        }
        for &s in samples {
            g.push_back(s as f32 / 32768.0);
        }
    }
}
