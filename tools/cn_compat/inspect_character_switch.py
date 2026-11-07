"""Read-only CN RAM inspection for character-switch research (no guest calls).

Run from the repository root:
  python tools/cn_compat/inspect_character_switch.py capture.bin --target 1
The report describes resource readiness, NOT permission to replace a live player.
"""
import argparse
import json
import struct
from pathlib import Path

NAMES = ('Beat', 'Corn', 'Gum', 'Combo', 'Bis', 'Soda', 'Yoyo', 'YoyoB',
         'Galam', 'Sugar', 'Cube', 'LShocker', 'PJam', 'NTank', 'Immortal',
         'HellMets', 'Rapid99', 'Pots', 'Gouji', 'TypeA', 'Clutch', 'Jazz',
         'TypeB', 'ZeroBeat')


class Capture:
    def __init__(self, data):
        self.data = data

    def check(self, address, size):
        if address < 0x10000 or address + size > min(len(self.data), 0x4000000):
            raise ValueError(f'invalid CN guest range: 0x{address:X} + 0x{size:X}')

    def word(self, address):
        self.check(address, 4)
        return struct.unpack_from('<I', self.data, address)[0]

    def chain(self, head, size, link):
        seen = set()
        while head:
            self.check(head, size)
            if head in seen or len(seen) >= 256:
                raise ValueError('cyclic or excessively long resource list')
            seen.add(head)
            yield head
            head = self.word(head + link)


def inspect(data, target):
    if not 0 <= target < len(NAMES):
        raise ValueError('target must be a base character ID from 0 to 23')
    m = Capture(data)
    root = m.word(0x22FCE0)
    m.check(root, 0x8800)
    obj = lambda index: m.word(root + 0x98 + index * 4)
    var = lambda index: m.word(root + 0x7868 + index * 4)
    res = obj(7)
    m.check(res, 0x278)
    shared = {m.word(p): p for p in m.chain(m.word(res + 0x1F0), 0x550, 0x54C)}
    records = list(m.chain(m.word(res + 0x1E4), 0x660, 0x65C))
    # Match the original getter: exact ID first, then record.ID & 0xFF.
    record = next((p for p in records if m.word(p + 8) == target), 0)
    if not record:
        record = next((p for p in records if m.word(p + 8) & 0xFF == target), 0)
    model = properties = 0
    if record:
        bank = shared.get(m.word(record + 0x10), 0)
        model = m.word(record + 0x640) or (m.word(bank + 0x530) if bank else 0)
        properties = m.word(record + 0x644) or (m.word(bank + 0x534) if bank else 0)
    for pointer in (model, properties):
        if pointer:
            m.check(pointer, 4)
    camera = obj(0x4C)
    player = 0
    if camera:
        m.check(camera, 0x28)
        player = m.word(camera + 0x24)
    if player:
        m.check(player, 0xFC0)
    flow = obj(9)
    if flow:
        m.check(flow, 0x60)
    unlocked = m.word(0x1EFFB0 + 0x14)
    busy = m.word(res + 0x44) != 0
    is_unlocked = bool(unlocked & (1 << target))
    exact = any(m.word(p + 8) == target for p in records)
    ready = exact and bool(model and properties) and not busy
    action = ('locked' if not is_unlocked else 'wait_loader' if busy else
              'resources_ready' if ready else 'preload_command_2' if not exact else
              'incomplete_resources')
    return {
        'profile': 'CN only; address layout must match this executable',
        'target': {'id': target, 'name': NAMES[target], 'unlocked': is_unlocked},
        'unlocked': [{'id': i, 'name': name} for i, name in enumerate(NAMES)
                     if unlocked & (1 << i)],
        'current_player_id': m.word(player + 0xFBC) if player else None,
        'selected_id': var(0xD), 'spawn_id': var(0x2D),
        'pending_spawn': var(0x6D),
        'flow_state': m.word(flow + 0x5C) if flow else None,
        'loader_busy': busy,
        'resident_ids': [m.word(p + 8) for p in records],
        'target_exact_resident': exact,
        'resolved_model': model, 'resolved_properties': properties,
        'resource_action': action,
        'live_swap_verified': False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--target', required=True, type=int, choices=range(24))
    args = parser.parse_args()
    try:
        result = inspect(args.capture.read_bytes(), args.target)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    print(json.dumps(result, ensure_ascii=True, indent=2))


if __name__ == '__main__':
    main()
