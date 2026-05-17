#!/usr/bin/env python3
"""
Marc Lab — Gel Imaging Timelapse
Raspberry Pi camera timelapse for gel electrophoresis monitoring.

Usage:
    python timelapse.py --interval 30 --run-name my_gel_run
    python timelapse.py --interval 60 --duration 3600 --stitch
    python timelapse.py --interval 30 --discord-webhook https://discord.com/api/webhooks/...

Dependencies (Pi OS Bookworm):
    picamera2   — pre-installed on Pi OS Bookworm; 'sudo apt install python3-picamera2' if missing
    requests    — 'pip install requests'
    ffmpeg      — 'sudo apt install ffmpeg'  (only needed with --stitch)
"""

import argparse
import datetime
import json
import pathlib
import subprocess
import sys
import time

# ─── DEPENDENCY CHECK ────────────────────────────────────────────────────────

def check_dependencies(need_discord: bool, need_stitch: bool) -> None:
    missing = []

    try:
        from picamera2 import Picamera2  # noqa: F401
    except ImportError:
        missing.append("picamera2  →  sudo apt install python3-picamera2")

    if need_discord:
        try:
            import requests  # noqa: F401
        except ImportError:
            missing.append("requests   →  pip install requests")

    if need_stitch:
        result = subprocess.run(["which", "ffmpeg"], capture_output=True)
        if result.returncode != 0:
            missing.append("ffmpeg     →  sudo apt install ffmpeg")

    if missing:
        print("Missing dependencies:")
        for m in missing:
            print(f"  {m}")
        sys.exit(1)


# ─── DISCORD ─────────────────────────────────────────────────────────────────

def post_to_discord(webhook_url: str, image_path: pathlib.Path,
                    frame_index: int, run_name: str) -> None:
    try:
        import requests
        with open(image_path, "rb") as f:
            payload = {"content": f"`{run_name}` — frame {frame_index:05d}"}
            files = {"file": (image_path.name, f, "image/jpeg")}
            resp = requests.post(webhook_url, data=payload, files=files, timeout=10)
            resp.raise_for_status()
    except Exception as e:
        # Never let network issues kill the capture loop
        print(f"[discord] post failed (frame {frame_index}): {e}")


# ─── MP4 STITCH ──────────────────────────────────────────────────────────────

def stitch_to_mp4(frames_dir: pathlib.Path, output_path: pathlib.Path,
                  fps: int = 2) -> None:
    print(f"Stitching frames → {output_path} at {fps} fps ...")
    cmd = [
        "ffmpeg", "-y",
        "-framerate", str(fps),
        "-pattern_type", "glob",
        "-i", str(frames_dir / "frame_*.jpg"),
        "-c:v", "libx264",
        "-pix_fmt", "yuv420p",
        str(output_path),
    ]
    subprocess.run(cmd, check=True)
    print(f"Timelapse saved: {output_path}")


# ─── MAIN ────────────────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Marc Lab gel imaging timelapse")
    p.add_argument("--interval", type=float, default=30.0,
                   help="Capture interval in seconds (default: 30)")
    p.add_argument("--output-dir", type=pathlib.Path,
                   default=pathlib.Path.home() / "gel-runs",
                   help="Root directory for run output (default: ~/gel-runs)")
    p.add_argument("--run-name", type=str, default=None,
                   help="Run label (default: ISO timestamp)")
    p.add_argument("--duration", type=float, default=None,
                   help="Total run time in seconds (default: run until Ctrl-C)")
    p.add_argument("--discord-webhook", type=str, default=None,
                   help="Discord webhook URL for remote frame posting")
    p.add_argument("--discord-interval", type=int, default=5,
                   help="Post every Nth frame to Discord (default: 5)")
    p.add_argument("--stitch", action="store_true",
                   help="Auto-stitch frames to MP4 when run ends")
    p.add_argument("--fps", type=int, default=2,
                   help="MP4 frame rate when stitching (default: 2)")
    return p.parse_args()


def main() -> None:
    args = parse_args()

    check_dependencies(
        need_discord=args.discord_webhook is not None,
        need_stitch=args.stitch,
    )

    from picamera2 import Picamera2

    # ── Run directory setup ───────────────────────────────────────────────────
    run_name = args.run_name or datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    run_dir = args.output_dir / run_name
    frames_dir = run_dir / "frames"
    frames_dir.mkdir(parents=True, exist_ok=True)

    metadata_path = run_dir / "run_metadata.json"
    log_path = run_dir / "run_log.jsonl"

    start_time = datetime.datetime.now()
    metadata = {
        "run_name": run_name,
        "start_time": start_time.isoformat(),
        "interval_s": args.interval,
        "duration_s": args.duration,
        "discord_enabled": args.discord_webhook is not None,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2))
    print(f"Run: {run_name}")
    print(f"Output: {run_dir}")
    print(f"Interval: {args.interval}s", end="")
    if args.duration:
        n_frames = int(args.duration / args.interval)
        print(f"  |  Duration: {args.duration}s (~{n_frames} frames)")
    else:
        print("  |  Duration: until Ctrl-C")

    # ── Camera init ───────────────────────────────────────────────────────────
    cam = Picamera2()
    config = cam.create_still_configuration(main={"size": cam.sensor_resolution})
    cam.configure(config)
    cam.start()
    time.sleep(2)  # allow auto-exposure to settle

    print("Camera ready. Starting capture loop ...\n")

    frame_index = 0
    end_time = (start_time.timestamp() + args.duration) if args.duration else None

    try:
        while True:
            if end_time and time.time() >= end_time:
                print("Duration reached. Stopping.")
                break

            capture_start = time.monotonic()
            frame_path = frames_dir / f"frame_{frame_index:05d}.jpg"

            cam.capture_file(str(frame_path))
            ts = datetime.datetime.now().isoformat()

            # Log frame record
            record = {"frame": frame_index, "timestamp": ts, "file": frame_path.name}
            with open(log_path, "a") as lf:
                lf.write(json.dumps(record) + "\n")

            print(f"[{ts}] frame {frame_index:05d} → {frame_path.name}")

            # Discord post
            if (args.discord_webhook
                    and frame_index % args.discord_interval == 0):
                post_to_discord(args.discord_webhook, frame_path,
                                frame_index, run_name)

            frame_index += 1

            # Sleep for remainder of interval (account for capture time)
            elapsed = time.monotonic() - capture_start
            sleep_for = max(0.0, args.interval - elapsed)
            time.sleep(sleep_for)

    except KeyboardInterrupt:
        print("\nCapture stopped by user.")

    finally:
        cam.stop()
        cam.close()

        end_ts = datetime.datetime.now().isoformat()
        metadata["end_time"] = end_ts
        metadata["total_frames"] = frame_index
        metadata_path.write_text(json.dumps(metadata, indent=2))

        print(f"\nRun complete: {frame_index} frames captured.")
        print(f"Output: {run_dir}")

        if args.stitch and frame_index > 0:
            mp4_path = run_dir / f"{run_name}.mp4"
            stitch_to_mp4(frames_dir, mp4_path, fps=args.fps)


if __name__ == "__main__":
    main()
