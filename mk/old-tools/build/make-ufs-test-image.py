#!/usr/bin/env python3
"""Create a unified UFS fixture with an explicit optional persistence profile."""
from pathlib import Path
import argparse
from ufs_format import create


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    parser.add_argument('--format', choices=('ufs',), default='ufs')
    parser.add_argument('--size-mib', type=int, default=16)
    parser.add_argument('--root', type=Path)
    parser.add_argument('--cylinder-groups', type=int, default=2)
    parser.add_argument('--profile', choices=('ordinary', 'journal-snapshot'), default='ordinary')
    args = parser.parse_args()
    payload = create(args.size_mib * 1024 * 1024, args.root,
                     args.cylinder_groups, args.profile)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)


if __name__ == '__main__':
    main()
