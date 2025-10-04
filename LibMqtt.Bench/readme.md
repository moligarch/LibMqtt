# LibMqtt.Bench

High-throughput benchmark and latency profiler for the **LibMqtt** MQTT publishing interfaces.

It exercises three publishing strategies (and their `Endpoint` wrappers):

- **pub_p0** — `PoolZeroPublisher` (single connection, zero queue)
- **pub_pq** — `PoolQueuePublisher` (bounded ring + worker pool, policy-driven)
- **pub_multi** — `MultiClientPublisher` (N independent connections, no queue)
- **ep_\*** — same strategies driven via `Endpoint`

The tool measures:

- **Send throughput** (messages/s) and **receive throughput** (messages/s)
- **Local publish latency** (QoS ≥ 1): caller → broker ACK
- **End-to-end latency**: publish → subscriber receive

---

## Quick Start

1. Build **LibMqtt** and **LibMqtt.Bench** (Visual Studio, x64 **Release**).
2. Ensure your MQTT broker is reachable and (for TLS) that a CA file is available.
3. Run:
```
LibMqtt.Bench.exe `
    --broker mqtts://<broker_ip_address>:8883 `
    --topic bench/topic `
    --qos 1 `
    --msg 10000 `
    --size 100 `
    --threads 10 `
    --qcap 64 `
    --pubs 5 `
    --user testuser `
    --pass testpass `
    --cafile ca.crt
```
The benchmark runs all supported modes and prints a summary table.

---

## Build Notes (Windows / Visual Studio)

- Solution contains separate projects: `LibMqtt`, `LibMqtt.Bench`, `LibMqtt.Tests`.
- **Paho MQTT C++** is a dependency of `LibMqtt`.
- If you see a vcpkg manifest warning, enable manifests for the solution or pass:

    msbuild /p:VcpkgEnableManifest=true

- Ensure TLS certs (e.g., `ca.crt`) are copied or referenced by absolute path.

---

## CLI Options

| Option      | Type   | Default       | Description                                                                 |
|-------------|--------|---------------|-----------------------------------------------------------------------------|
| `--broker`  | string | **(required)**| Broker URI. Supports `mqtt://host:port` and `mqtts://host:port`.           |
| `--topic`   | string | `bench/topic` | Topic used for all modes.                                                   |
| `--qos`     | 0/1/2  | `1`           | MQTT QoS level.                                                             |
| `--msg`     | int    | `2000`        | Messages per mode (for `pub_multi`/`ep_multi`, total messages = `--msg`).  |
| `--size`    | int    | `100`         | Payload size in bytes.                                                      |
| `--threads` | int    | `10`          | Worker threads for pq/ep_pq.                                                |
| `--qcap`    | int    | `64`          | Ring capacity per worker for pq/ep_pq.                                      |
| `--pubs`    | int    | `5`           | Connection count for multi-client modes (pub_multi/ep_multi).               |
| `--user`    | string | (empty)       | Username (if broker requires auth).                                         |
| `--pass`    | string | (empty)       | Password (if broker requires auth).                                         |
| `--cafile`  | path   | (empty)       | CA file path for `mqtts://` (TLS).                                          |

> The benchmark intentionally runs **all modes** to make comparisons easier.

---

## What Each Mode Does

- **pub_p0** — Single connection, no internal queue.  
  - QoS 0: fire-and-forget; highest send rate; may drop on broker hiccups.  
  - QoS ≥ 1: `Publish()` waits for broker ACK (low latency, reliable).

- **pub_pq** — Worker pool with bounded ring per worker.  
  - Backpressure policy: `Block` / `DropNewest` / `DropOldest` (configured in code or defaults).  
  - Disconnect policy: `Wait` / `Drop`.  
  - Good for sustained bursts; trades higher e2e latency for smoothing under load.

- **pub_multi** — N independent connections (no queue).  
  - Spreads load across connections; best throughput for QoS ≥ 1; low e2e latency.

- **ep_\*** — The same strategies via the high-level `Endpoint` façade.

---

## Output & Interpreting Results

You’ll see a per-mode section and a final summary like:
```
==================== SUMMARY ====================
Mode      Thr_send  Thr_recv Recv/Total  p50_e2e  p95_e2e  p99_e2e  p50_local   p95_local   p99_local
pub_p0     24352.4    1782.5  2000/2000    40.30    64.74  1039.92       0.00        0.00        0.00
pub_pq     13208.0    9985.6  2000/2000    79.90   151.23   152.97       0.00        0.00        0.00
pub_multi  20445.2   11465.4  1959/2000    73.04    78.24    78.84       0.00        0.00        0.00
```

- **Thr_send** — messages/s produced by the publisher(s).
- **Thr_recv** — messages/s observed by the in-process subscriber.
- **Recv/Total** — delivery ratio; for QoS 0, drops are **expected** under load or reconnects.
- **Local latency (QoS ≥ 1)** — time from `Publish()` start to broker ACK (PUBACK/PUBCOMP).
- **End-to-end latency** — publish → receive measured by matching message ids between sender and subscriber.

### Typical Patterns

- **QoS 0**: Highest send rate; possible drops; e2e latencies reflect broker/network buffers.
- **QoS 1**: Multi-client tends to be best (balanced throughput, low latency).
- **QoS 2**: Throughput lower (exactly-once handshake), multi-client still wins; queue mode prioritizes smoothing, not minimal latency.

---

## Recipes

### TLS + Auth (QoS 1)

    LibMqtt.Bench.exe `
      --broker mqtts://10.0.0.5:8883 `
      --topic bench/topic `
      --qos 1 `
      --msg 10000 `
      --size 100 `
      --threads 10 `
      --qcap 64 `
      --pubs 5 `
      --user testuser `
      --pass testpass `
      --cafile C:\certs\ca.crt

### Best-Effort Firehose (QoS 0)

    LibMqtt.Bench.exe `
      --broker mqtts://10.0.0.5:8883 `
      --topic bench/topic `
      --qos 0 `
      --msg 20000 `
      --size 500 `
      --pubs 5 `
      --cafile C:\certs\ca.crt

---

## How Latencies Are Measured

- **Local (QoS ≥ 1)**: measured around the synchronous publish / `token->wait()` (caller → broker ACK).  
  For QoS 0 there is no ACK by definition; the local metric is reported as `0.00`.

- **End-to-end**: the sender stamps messages with a sequence/time marker; the subscriber timestamps receipt; the tool computes percentiles (p50/p95/p99).

> Because the publisher and subscriber run in the same process, clocks are naturally consistent.

---

## Choosing a Mode

- **Lowest latency, reliable (QoS ≥ 1)** → **pub_multi / ep_multi** (tune `--pubs` to saturate cores).
- **Simple & reliable (QoS ≥ 1)** → **pub_p0 / ep_p0** (sequential, very predictable).
- **Bursty producers with backpressure policy** → **pub_pq / ep_pq** (tune `--threads`, `--qcap`, and policy in code).

---

## Best Practices for Fair Runs

- Keep broker and benchmark on stable, uncongested hosts.
- Use a constant topic across modes for comparability.
- For TLS, place the CA file locally and verify the file path.
- Disable aggressive CPU power-saving if you need maximal reproducibility (optional).
- Avoid other heavy workloads on the same machine during a run.

---

## Troubleshooting

- **`--> ERROR: connect failed (rc=-1)`**  
  Check broker address/port, firewall, credentials, and `--cafile` (for TLS).

- **High QoS 0 drops**  
  Expected under load; use QoS 1 for delivery guarantees.

---

## Notes on Policies (Queue Mode)

Queue mode uses a bounded ring per worker with configurable policies (set in code):

- **OverflowPolicy**
  - `Block`: apply backpressure (highest reliability)
  - `DropNewest`: reject incoming when full
  - `DropOldest`: overwrite the oldest (LRU-like freshness)

- **DisconnectPolicy**
  - `Wait`: wait for connection (reliable)
  - `Drop`: discard tasks encountered while disconnected (QoS 0 perf mode)

`Flush()` drains the rings and, for QoS ≥ 1, waits for all broker ACKs before returning.

---

## License

This benchmark is part of the **LibMqtt** repository and follows the repo’s license.

---