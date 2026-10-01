"""Cross-check the C subnet calculator against Python's ipaddress module."""

import ipaddress
from pathlib import Path
import subprocess
import sys


def main():
    binary = Path(sys.argv[1]).resolve(strict=True)
    output = subprocess.run(
        [str(binary), "--vectors"], check=True, capture_output=True, text=True, timeout=30
    ).stdout
    rows = output.splitlines()
    if len(rows) != 33 * 64:
        raise AssertionError(f"Expected 2112 vectors, got {len(rows)}")
    for row in rows:
        address, peer, *actual = row.split(",")
        interface = ipaddress.IPv4Interface(address)
        network = interface.network
        broadcast = network.prefixlen <= 30

        def role(value):
            if value not in network:
                return 3
            if broadcast and value == network.network_address:
                return 1
            if broadcast and value == network.broadcast_address:
                return 2
            return 0

        expected = [
            int(network.network_address), int(network.broadcast_address),
            int(network.netmask), int(network.hostmask),
            int(next(iter(network.hosts()))), int(network[-2 if broadcast else -1]),
            network.num_addresses, network.num_addresses - (2 if broadcast else 0),
            int(broadcast), role(interface.ip), role(ipaddress.IPv4Address(peer)),
        ]
        if list(map(int, actual)) != expected:
            raise AssertionError(f"{address}, peer {peer}: {actual} != {expected}")
    print(f"Subnet reference: {len(rows)} vectors across /0-/32 match Python ipaddress PASS")


if __name__ == "__main__":
    main()
