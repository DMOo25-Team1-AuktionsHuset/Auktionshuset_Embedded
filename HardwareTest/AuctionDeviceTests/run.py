#!/usr/bin/env python3
"""Compatibility launcher; bash and g++ are required."""
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[2]
subprocess.run(['bash', str(root / 'HardwareTest/AuctionDeviceTests/run.sh'), str(root)], check=True)
