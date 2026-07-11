#!/usr/bin/env bash
set -euo pipefail

cd /home/nvidia/DAP
source /home/nvidia/miniconda3/etc/profile.d/conda.sh
conda activate dap

pkill -f tcp_dap_point_receiver.py 2>/dev/null || true
mkdir -p /home/nvidia/DAP/data/rgb /home/nvidia/DAP/data/depth /home/nvidia/DAP/data/point

nohup python /home/nvidia/DAP/tcp_dap_point_receiver.py \
  --host 0.0.0.0 \
  --port 5001 \
  --rgb-dir /home/nvidia/DAP/data/rgb \
  --depth-dir /home/nvidia/DAP/data/depth \
  --point-dir /home/nvidia/DAP/data/point \
  > /tmp/dap_tcp_receiver.log 2>&1 &

sleep 2
pgrep -af tcp_dap_point_receiver.py
ss -ltnp | grep ':5001' || true
tail -40 /tmp/dap_tcp_receiver.log
